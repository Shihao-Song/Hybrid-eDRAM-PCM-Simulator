#ifndef __SIMULATION_HH__
#define __SIMULATION_HH__

#include <boost/program_options.hpp>
#include <cstring>

#include "Sim/config.hh"
#include "Sim/stats.hh"
#include "Sim/trace.hh"

#include "System/mmu_factory.hh"

#include "CacheSim/cache.hh"
#include "PCMSim/Memory_System/pcm_sim_memory_system.hh"
#include "Processor/processor.hh"

#define isLLC 1
#define isNonLLC 0

typedef Simulator::Config Config;
typedef Simulator::MemObject MemObject;
typedef Simulator::Request Request;
typedef Simulator::Stats Stats;

typedef CoreSystem::Processor Processor;

enum class Memories : int
{
    L1_I_CACHE,
    L1_D_CACHE,
    L2_CACHE,
    L3_CACHE,
    eDRAM,
    PCM
};

struct ParseArgsRet
{
    std::vector<std::string> cfg_files;
    std::vector<std::string> trace_lists;
    int64_t num_instrs_per_phase;
    std::string stats_output_file;
    std::string offline_request_analysis_file;
};
ParseArgsRet parse_args(int argc, const char *argv[]);

auto createTrainedMMU(int num_of_cores, Config &cfg)
{
    return System::createTrainedMMU(num_of_cores, cfg);
}

auto createHybridSystem(Config &dram_cfg,
                        Config &pcm_cfg)
{
    return PCMSim::createHybridSystem(dram_cfg, pcm_cfg);
}

auto createMemObject(Config &cfg,
                     Memories mem_type,
                     bool LLC = false)
{
    if (mem_type == Memories::PCM)
    {
        return PCMSim::createPCMSimMemorySystem(cfg);
    }
    else
    {
        if (mem_type == Memories::L1_I_CACHE)
        {
            return CacheSimulator::createCache(Config::Cache_Level::L1I, cfg);
        }
        else if (mem_type == Memories::L1_D_CACHE)
        {
            return CacheSimulator::createCache(Config::Cache_Level::L1D, cfg);
        }
        else if (mem_type == Memories::L2_CACHE)
        {
            return CacheSimulator::createCache(Config::Cache_Level::L2, cfg, LLC);
        }
        else if (mem_type == Memories::L3_CACHE)
        {
            return CacheSimulator::createCache(Config::Cache_Level::L3, cfg, LLC);
        }
        else if (mem_type == Memories::eDRAM)
        {
            return CacheSimulator::createCache(Config::Cache_Level::eDRAM, cfg, LLC);
        }
    }
}

auto runCPUTrace(Processor *processor)
{
    while (!processor->done())
    {
        processor->tick();
    }
}

ParseArgsRet parse_args(int argc, const char *argv[])
{
    std::vector<std::string> cfg_files;
    std::vector<std::string> cpu_traces;
    int64_t num_instrs_per_phase = -1;
    std::string stats_output;
    std::string offline_request_analysis_file;

    namespace po = boost::program_options;
    po::options_description desc("Options"); 
    desc.add_options() 
        ("help", "Print help messages")
        ("config", po::value<std::vector<std::string>>(&cfg_files)->required(), 
                   "Configuration file")
        ("cpu_trace", po::value<std::vector<std::string>>(&cpu_traces)->required(),
                      "CPU trace")
        ("num_instrs_per_phase", po::value<int64_t>(&num_instrs_per_phase),
                   "Number of instructions per phase (Optional)")
        ("stat_output", po::value<std::string>(&stats_output)->required(),
                        "Stats output file")
        ("offline_request_analysis_file",
            po::value<std::string>(&offline_request_analysis_file),
            "Offline request analysis file");

    po::variables_map vm;

    try 
    { 
        po::store(po::parse_command_line(argc, argv, desc), vm); // can throw 
 
        if (vm.count("help")) 
        { 
            std::cout << "A CPU-trace driven PCM Simulator.\n" 
                      << desc << "\n"; 
            exit(0);
        } 

        po::notify(vm);	
    } 
    catch(po::error& e) 
    { 
        std::cerr << "ERROR: " << e.what() << "\n\n"; 
        std::cerr << desc << "\n"; 
        exit(0);
    }

    return ParseArgsRet{cfg_files,
                        cpu_traces,
                        num_instrs_per_phase,
                        stats_output,
                        offline_request_analysis_file};
}

// Function to test cache behavior.
// Please also un-comment std::cout sections in corresponding tag class.
auto runCacheTest(const char* cfg_file, const char *trace_name)
{
    Config cfg(cfg_file);
    Simulator::Trace cpu_trace(trace_name);
    
    Simulator::Instruction instr;

    // To test Set-Assoc tag with LRU replacement policy.
    CacheSimulator::LRUSetWayAssocTags tags(int(Config::Cache_Level::L1D), cfg);
    tags.printTagInfo();

    std::cout << "\nCache (tag) stressing mode...\n";

    uint64_t cycles = 0;

    uint64_t num_evictions = 0;
    uint64_t num_hits = 0;
    uint64_t num_misses = 0;

    bool more_insts = cpu_trace.getInstruction(instr);
    while (more_insts)
    {
        if (instr.opr == Simulator::Instruction::Operation::LOAD ||
            instr.opr == Simulator::Instruction::Operation::STORE)
        {
            
            uint64_t addr = instr.target_vaddr;
            if (auto [hit, aligned_addr] = tags.accessBlock(addr,
                                       instr.opr == Simulator::Instruction::Operation::STORE ?
                                       true : false,
                                       cycles);
                !hit)
            {
                ++num_misses;
                if (auto [wb_required, wb_addr] = tags.insertBlock(aligned_addr,
                                       instr.opr == Simulator::Instruction::Operation::STORE ?
                                       true : false,
                                       cycles);
                    wb_required)
		{
                    ++num_evictions;
                }
            }
            else
            {
                ++num_hits;
            }
        }
        more_insts = cpu_trace.getInstruction(instr);
        ++cycles;
    }
    double hit_rate = (double)num_hits / ((double)num_misses + (double)num_hits);
    std::cout << "Hit rate: " << hit_rate << "\n";
    std::cout << "Number of evictions: " << num_evictions << "\n";
}

// This function is used to extract traces to LLC. You can use this function to do off-line
// LLC replacement policy or prefetcher study.
auto LLCTrace(Config &cfg, std::vector<std::string> &trace_lists, std::string output)
{
    std::ofstream trace_out(output);

    // Each cores runs the same workload but different virtual-physical address mappings.
    const unsigned num_of_cores = 4;

    // Create L1_D for each core.
    std::vector<CacheSimulator::LRUSetWayAssocTags> L1_Ds;
    for (int i = 0; i < num_of_cores; i++)
    {
        L1_Ds.emplace_back(int(Config::Cache_Level::L1D), cfg);
    }

    // Create an MMU for all the cores
    System::MMU mmu(num_of_cores); 

    // Trace parser
    Simulator::Trace cpu_trace(trace_lists[0]);

    // Simulation
    uint64_t cycles = 0;

    uint64_t num_evictions = 0;
    uint64_t num_hits = 0;
    uint64_t num_misses = 0;

    uint64_t block_mask = 63;

    Simulator::Instruction cur_inst;
    bool more_instructions = cpu_trace.getInstruction(cur_inst);
    while (more_instructions)
    {
        int core_id = 0;
        for (auto &L1_D : L1_Ds)
        {
            if (cur_inst.opr == Simulator::Instruction::Operation::LOAD ||
                cur_inst.opr == Simulator::Instruction::Operation::STORE)
            {
                // Prepare a request packet for translation
                Request req;
                if (cur_inst.opr == Simulator::Instruction::Operation::LOAD)
                {
                    req.req_type = Request::Request_Type::READ;
                }
                else if (cur_inst.opr == Simulator::Instruction::Operation::STORE)
                {
                    req.req_type = Request::Request_Type::WRITE;
                }

                req.core_id = core_id;
                req.eip = cur_inst.eip;
                req.addr = cur_inst.target_vaddr; // assign virtual address first
                // Address translation
                mmu.va2pa(req);
                req.addr = req.addr & (~block_mask); // align the address

                // Send to cache
                if (auto [hit, aligned_addr] =
                        L1_D.accessBlock(req.addr, // translated physical address
                                         req.req_type == Request::Request_Type::WRITE ?
                                                         true : false,
                                         cycles);
                    !hit)
                {
                    ++num_misses;
                    // This is a miss.
		    // Step one, need to load the missing block from the next level.
		    trace_out << req.core_id << " " << req.eip << " "
                              << aligned_addr << " L\n";

                    // Step two, insert the block into cache
                    if (auto [wb_required, wb_addr] = 
                            L1_D.insertBlock(aligned_addr,
                                             req.req_type == Request::Request_Type::WRITE ?
                                             true : false,
                                             cycles);
                        wb_required)
                    {
                        // Step three, evict the block to next level
                        auto [wb_core_id, 
                              wb_eip,
                              wb_mmu_commu] = L1_D.retriMMUCommu(aligned_addr);

                        trace_out << wb_core_id << " " << wb_eip << " "
                                  << wb_addr << " S\n";
                        ++num_evictions;
                    }

                    // Record the instruction that brings in this cache block
                    L1_D.recordMMUCommu(aligned_addr, req.core_id, req.eip, 0);
                }
                else
                {
                    ++num_hits;
                }
            }

            ++core_id; 
        }
        ++cycles;
	more_instructions = cpu_trace.getInstruction(cur_inst);
    }

    trace_out.close();
    std::cout << "Number of misses: " << num_misses << "\n";
    std::cout << "Number of evictions: " << num_evictions << "\n";
}

auto runMemTrace(MemObject *mem_obj,
                 const char *trace_name,
                 System::TrainedMMU *mmu)
{
    Simulator::Trace mem_trace(trace_name);

    Request req;

    std::cout << "\nMemory-trace driven simulation...\n";
    uint64_t Tick = 0;
    bool stall = false;
    bool end = false;

    while (!end || mem_obj->pendingRequests())
    {
        if (!end && !stall)
        {
            end = !(mem_trace.getMemtraceRequest(req));
        }

        if (!end)
        {
            stall = !(mem_obj->send(req));
        }

        mem_obj->tick();
        ++Tick;
    }

    return Tick;
}

#endif
