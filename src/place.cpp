#include "place.hpp"

#include <fstream>
#include <chrono>

#ifdef __OMP
#include <omp.h>
#endif

#include "file_io.hpp"
#include "jplace_util.hpp"
#include "stringify.hpp"
#include "set_manipulators.hpp"
#include "Log.hpp"
#include "Tiny_Tree.hpp"
#include "mpihead.hpp"
#include "pll_util.hpp"
#include "epa_pll_util.hpp"
#include "Timer.hpp"
#include "Work.hpp"
#include "schedule.hpp"

#ifdef __MPI
#include "epa_mpi_util.hpp"
#endif

typedef std::vector<std::vector<std::vector<double>>> lookupstore_t;

static void place(const Work& to_place, MSA_Stream& msa, Tree& reference_tree,
  const std::vector<pll_utree_t *>& branches, Sample& sample,
  bool do_blo, const Options& options, lookupstore_t& lookup_store)
{

#ifdef __OMP
  unsigned int num_threads = options.num_threads ? options.num_threads : omp_get_max_threads();
#else
  unsigned int num_threads = 1;
#endif

  // split the sample structure such that the parts are thread-local
  std::vector<Sample> sample_parts(num_threads);
  std::vector<Work> work_parts;
  split(to_place, work_parts, num_threads);

  // work seperately
#ifdef __OMP
  #pragma omp parallel for schedule(dynamic)
#endif
  for (size_t i = 0; i < work_parts.size(); ++i)
  {
    for (const auto& pair : work_parts[i])
    {
      auto branch_id = pair.first;
      auto branch = Tiny_Tree(branches[branch_id], branch_id, reference_tree, do_blo, options, lookup_store[branch_id]);

      for (const auto& seq_id : pair.second)
        sample_parts[i].add_placement(seq_id, branch.place(msa[seq_id]));
    }
  }
  // merge samples back
  merge(sample, sample_parts);
}

void process(Tree& reference_tree, MSA_Stream& msa_stream, const std::string& outdir,
              const Options& options, const std::string& invocation)
{
  /* ===== COMMON DEFINITIONS ===== */
  int local_rank = 0;

#ifdef __MPI
  int world_size;
  MPI_Comm_rank(MPI_COMM_WORLD, &local_rank);
  MPI_Comm_size(MPI_COMM_WORLD, &world_size);

  lgr.dbg() << "World Size: " << world_size << std::endl;

  // shuffle available nodes to the different stages
  std::vector<std::vector<int>> schedule;
  int local_stage;
  const unsigned int num_stages = options.prescoring ? 4 : 2;

  lgr.dbg() << "Stages: " << num_stages << std::endl;

  unsigned int rebalance = 3;
  unsigned int rebalance_delta = rebalance;
  bool reassign_happened = true;

  std::vector<double> init_diff = options.prescoring
    ? std::vector<double>{1000.0, 1.0, 1000.0, 1.0} : std::vector<double>{1000.0, 1.0};

  // get initial schedule
  auto init_nps = solve(num_stages, world_size, init_diff);
  assign(local_rank, init_nps, schedule, &local_stage);

  lgr.dbg() << "Schedule: ";
  for (size_t i = 0; i < schedule.size(); ++i)
  {
    lgr.dbg() << schedule[i].size() << " ";
  }
  lgr.dbg() << std::endl;

  Timer timer;

  const auto EPA_MPI_STAGE_LAST_AGGREGATE
    = options.prescoring ? EPA_MPI_STAGE_2_AGGREGATE : EPA_MPI_STAGE_1_AGGREGATE;
  const auto EPA_MPI_DEDICATED_WRITE_RANK = schedule[EPA_MPI_STAGE_LAST_AGGREGATE][0];

  previous_request_storage_t prev_requests;

#endif // __MPI
  lgr << "P-EPA - Massively-Parallel Evolutionary Placement Algorithm\n";
  lgr << "\nInvocation: \n" << invocation << "\n";

  const auto chunk_size = options.chunk_size;
  lgr.dbg() << "Chunk size: " << chunk_size << std::endl;
  unsigned int num_sequences;

  const auto num_branches = reference_tree.nums().branches;

  // get all edges
  std::vector<pll_utree_t *> branches(num_branches);
  auto num_traversed_branches = utree_query_branches(reference_tree.tree(), &branches[0]);
  if(num_traversed_branches != num_branches)
    throw std::runtime_error{"Traversing the utree went wrong during pipeline startup!"};

  double lowest = 1e15;
  for (size_t i = 0; i < num_branches; ++i)
  {
    if(branches[i]->length < lowest)
      lowest = branches[i]->length;
  }

  lgr.dbg() << "smallest BL: " << lowest << std::endl;

  unsigned int chunk_num = 1;
  Sample sample;

  lookupstore_t previously_calculated_lookups(num_branches);

  Work all_work(std::make_pair(0, num_branches), std::make_pair(0, chunk_size));
  Work first_placement_work;
  Work second_placement_work; // dummy structure to be filled during operation
  std::vector<std::string> part_names; // filenames of partial results

  // while data
  // TODO this could be a stream read, such that cat msa.fasta | epa ... would work
  while ((num_sequences = msa_stream.read_next(chunk_size)) > 0)
  {
    if (num_sequences < chunk_size)
      all_work = Work(std::make_pair(0, num_branches), std::make_pair(0, num_sequences));

#ifdef __MPI
    // timer.start(); // start timer of any stage
    //==============================================================
    // EPA_MPI_STAGE_1_COMPUTE === BEGIN
    //==============================================================
    if (local_stage == EPA_MPI_STAGE_1_COMPUTE)
    {
    // if previous chunk was a rebalance chunk or this is the first chunk (0 mod anything = 0)
    // ...then we need to correctly assign/reassign the workload of the first compute stage
    if ( reassign_happened )
    {
      lgr.dbg() << "Assigning first stage Work" << std::endl;
      const auto& stage = schedule[EPA_MPI_STAGE_1_COMPUTE];
      std::vector<Work> parts;
      split(all_work, parts, stage.size());
      // find the stage-relative rank
      auto it = std::find(stage.begin(), stage.end(), local_rank);
      size_t stage_rank = std::distance(stage.begin(), it);
      first_placement_work = parts[stage_rank];
      reassign_happened = false;
    }
    timer.start();
#else
    first_placement_work = all_work;
#endif //__MPI

    place(first_placement_work, msa_stream, reference_tree, branches, sample, !options.prescoring, options, previously_calculated_lookups);

#ifdef __MPI
    timer.stop();
    // MPI: split the result and send the part to correct aggregate node
    lgr.dbg() << "Sending Stage 1 Results..." << std::endl;
    epa_mpi_split_send(sample, num_sequences, schedule[EPA_MPI_STAGE_1_AGGREGATE], MPI_COMM_WORLD, prev_requests);
    lgr.dbg() << "Stage 1 Send done!" << std::endl;

    } // endif (local_stage == EPA_MPI_STAGE_1_COMPUTE)
    //==============================================================
    // EPA_MPI_STAGE_1_COMPUTE === END
    //==============================================================

    //==============================================================
    // EPA_MPI_STAGE_1_AGGREGATE === BEGIN
    //==============================================================
    if (local_stage == EPA_MPI_STAGE_1_AGGREGATE)
    {
    // (MPI: recieve results, merge them)
    lgr.dbg() << "Recieving Stage 1 Results..." << std::endl;
    epa_mpi_recieve_merge(sample, schedule[EPA_MPI_STAGE_1_COMPUTE], MPI_COMM_WORLD);
    lgr.dbg() << "Stage 1 Recieve done!" << std::endl;
    timer.start();
#endif // __MPI

    compute_and_set_lwr(sample);

    // if this was a prescring run, select the candidate edges
    if (options.prescoring)
    {
      if (options.prescoring_by_percentage)
        discard_bottom_x_percent(sample, (1.0 - options.prescoring_threshold));
      else
        discard_by_accumulated_threshold(sample, options.prescoring_threshold);
    }

    if(options.prescoring)
      second_placement_work = Work(sample);
#ifdef __MPI
    // if prescoring was selected, we need to send the intermediate results off to thorough placement
    if (options.prescoring)
    {
      timer.stop();
      epa_mpi_split_send(second_placement_work, schedule[EPA_MPI_STAGE_2_COMPUTE], MPI_COMM_WORLD, prev_requests);
    }
    } // endif (local_stage == EPA_MPI_STAGE_1_AGGREGATE)
    //==============================================================
    // EPA_MPI_STAGE_1_AGGREGATE === END
    //==============================================================

    //==============================================================
    // EPA_MPI_STAGE_2_COMPUTE === BEGIN
    //==============================================================
    if (local_stage == EPA_MPI_STAGE_2_COMPUTE and options.prescoring)
    {
    epa_mpi_recieve_merge(second_placement_work, schedule[EPA_MPI_STAGE_1_AGGREGATE], MPI_COMM_WORLD);
    timer.start();
#endif // __MPI
    if (options.prescoring)
    {
      place(second_placement_work, msa_stream, reference_tree, branches, sample, true, options, previously_calculated_lookups);
    }
#ifdef __MPI
    timer.stop();
    if(options.prescoring)
      epa_mpi_split_send(sample, num_sequences, schedule[EPA_MPI_STAGE_2_AGGREGATE], MPI_COMM_WORLD, prev_requests);

    } // endif (local_stage == EPA_MPI_STAGE_2_COMPUTE)
    //==============================================================
    // EPA_MPI_STAGE_2_COMPUTE === END
    //==============================================================

    //==============================================================
    // EPA_MPI_STAGE_2_AGGREGATE === BEGIN
    //==============================================================
    if (local_stage == EPA_MPI_STAGE_LAST_AGGREGATE)
    {
      // only if this is the 4th stage do we need to get from mpi
    if (local_stage == EPA_MPI_STAGE_2_AGGREGATE)
    {
      epa_mpi_recieve_merge(sample, schedule[EPA_MPI_STAGE_2_COMPUTE], MPI_COMM_WORLD);
      timer.start();
    }
#endif // __MPI
    // recompute the lwrs
    if (options.prescoring)
      compute_and_set_lwr(sample);
    // discard uninteresting placements
    if (options.acc_threshold)
      discard_by_accumulated_threshold(sample, options.support_threshold);
    else
      discard_by_support_threshold(sample, options.support_threshold);

    // write results of current last stage aggregator node to a part file
    std::string part_file_name(outdir + "epa." + std::to_string(local_rank)
      + "." + std::to_string(chunk_num) + ".part");
    std::ofstream part_file(part_file_name);
    part_file << sample_to_jplace_string(sample, msa_stream);
    part_names.push_back(part_file_name);
    part_file.close();

#ifdef __MPI
    timer.stop();
    } // endif aggregate cleanup

    //==============================================================
    // EPA_MPI_STAGE_2_AGGREGATE === END
    //==============================================================
    // timer.stop(); // stop timer of any stage

    if ( (chunk_num == rebalance) ) // time to rebalance
    {
      lgr.dbg() << "Rebalancing..." << std::endl;
      int foreman = schedule[local_stage][0];
      // Step 1: aggregate the runtime statistics, first at the lowest rank per stage
      lgr.dbg() << "aggregate the runtime statistics..." << std::endl;
      epa_mpi_gather(timer, foreman, schedule[local_stage], local_rank);
      lgr.dbg() << "Runtime aggregate done!" << std::endl;

      // Step 2: calculate average time needed per chunk for the stage
      std::vector<double> perstage_avg(num_stages);

      int color = (local_rank == foreman) ? 1 : MPI_UNDEFINED;
      MPI_Comm foreman_comm;
      MPI_Comm_split(MPI_COMM_WORLD, color, local_rank, &foreman_comm);

      if (local_rank == foreman)
      {
        double avg = timer.average();
        // Step 3: make known to all other stage representatives (mpi_allgather)
        lgr.dbg() << "Foremen allgather..." << std::endl;
        MPI_Allgather(&avg, 1, MPI_DOUBLE, &perstage_avg[0], 1, MPI_DOUBLE, foreman_comm);
        lgr.dbg() << "Foremen allgather done!" << std::endl;
        MPI_Comm_free(&foreman_comm);
      }
      // ensure all messages were recieved and previous requests are cleared
      epa_mpi_waitall(prev_requests);
      
      MPI_BARRIER(MPI_COMM_WORLD);
      // Step 4: stage representatives forward results to all stage members
      // epa_mpi_bcast(perstage_avg, foreman, schedule[local_stage], local_rank);
      lgr.dbg() << "Broadcasting..." << std::endl;
      MPI_Comm stage_comm;
      MPI_Comm_split(MPI_COMM_WORLD, local_stage, local_rank, &stage_comm);
      MPI_Bcast(&perstage_avg[0], num_stages, MPI_DOUBLE, 0, stage_comm);
      MPI_Comm_free(&stage_comm);
      lgr.dbg() << "Broadcasting done!" << std::endl;

      lgr.dbg() << "perstage average:";
      for (size_t i = 0; i < perstage_avg.size(); ++i)
      {
        lgr.dbg() << " " << perstage_avg[i];
      }
      lgr.dbg() << std::endl;

      // Step 5: calculate schedule on every rank, deterministically!
      to_difficulty(perstage_avg);

      lgr.dbg() << "perstage difficulty:";
      for (size_t i = 0; i < perstage_avg.size(); ++i)
      {
        lgr.dbg() << " " << perstage_avg[i];
      }
      lgr.dbg() << std::endl;

      auto nps = solve(num_stages, world_size, perstage_avg);
      reassign(local_rank, nps, schedule, &local_stage);
      // Step 6: re-engage pipeline with new assignments
      lgr.dbg() << "New Schedule:";
      for (size_t i = 0; i < nps.size(); ++i)
        lgr.dbg() << " " << nps[i];
      lgr.dbg() << std::endl;
      // compute stages should try to keep their edge assignment! affinity!
      lgr.dbg() << "Rebalancing done!" << std::endl;
      // exponential back-off style rebalance:
      rebalance_delta *= 2;
      rebalance += rebalance_delta;
      reassign_happened = true;
    }

    prev_requests.clear();
#endif // __MPI
    second_placement_work.clear();
    sample.clear();
    msa_stream.clear();
    lgr.dbg() << "Chunk " << chunk_num << " done!" << std::endl;
    chunk_num++;
  }


  //==============================================================
  // POST COMPUTATION
  //==============================================================
  lgr.dbg() << "Starting Post-Comp" << std::endl;
  // finally, paste all part files together
#ifdef __MPI
  MPI_BARRIER(MPI_COMM_WORLD);

  if (local_rank != EPA_MPI_DEDICATED_WRITE_RANK)
  {
    if (local_stage == EPA_MPI_STAGE_LAST_AGGREGATE)
      epa_mpi_send(part_names, EPA_MPI_DEDICATED_WRITE_RANK, MPI_COMM_WORLD);
  }
  else
  {
    for (auto rank : schedule[EPA_MPI_STAGE_LAST_AGGREGATE])
    {
      if (rank == local_rank)
        continue;
      std::vector<std::string> remote_obj;
      epa_mpi_recieve(remote_obj, rank, MPI_COMM_WORLD);
      part_names.insert(part_names.end(), remote_obj.begin(), remote_obj.end());
    }
#endif
    // create output file
    std::ofstream outfile(outdir + "epa_result.jplace");
    lgr << "\nOutput file: " << outdir + "epa_result.jplace" << std::endl;
    outfile << init_jplace_string(get_numbered_newick_string(reference_tree.tree()));
    merge_into(outfile, part_names);
    outfile << finalize_jplace_string(invocation);
    outfile.close();
#ifdef __MPI
  }
#endif
}

