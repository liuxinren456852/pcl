/*
* Software License Agreement (BSD License)
*
*  Copyright (c) 2011, Willow Garage, Inc.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of Willow Garage, Inc. nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*
*  Author: Anatoly Baskeheev, Itseez Ltd, (myname.mysurname@mycompany.com)
*/

#ifndef PCL_GPU_OCTREE_STA_INITIAL
#define PCL_GPU_OCTREE_STA_INITIAL

#include "cta_base.hpp"
#include "octree_global.hpp"
#include "builder/tasks_global.hpp"

#include "utils/scan_block.hpp"
#include "utils/copygen.hpp"


namespace pcl
{
    namespace device
    {
        namespace initial
        {    
            struct KernelInitialPolicyGlobOctree
            {   
                enum 
                { 
                    LEVEL_BITS_NUM = 3,
                    ARITY = 1 << LEVEL_BITS_NUM,

                    CTA_SIZE = 1024,
                    OFFSETS_SIZE = CTA_SIZE,
                    GRID_SIZE = 1
                };

                struct SmemStorage
                {           
                    int tasks_beg;
                    int tasks_end;

                    int offsets[CTA_SIZE];
                    int terminate;

                    int nodes_num;            
                };
            };  
                                               
            template<typename KernalPolicy>
            struct Cta_InitialGlobOctree : public base::Cta_base
            {
                typedef typename KernalPolicy::SmemStorage SmemStorage;

                SmemStorage& storage;

                __device__ __forceinline__ Cta_InitialGlobOctree(OctreeGlobal& octree_global_arg, const int *codes_arg, SmemStorage& storage_arg) 
                    : Cta_base(octree_global_arg, codes_arg), storage(storage_arg) {}       

                __device__ __forceinline__ int Build(int points_num)    
                {
                    int tid = threadIdx.x;

                    if (tid == 0)
                    {
                        //init root
                        octree_global.codes[0] = 0;
                        octree_global.nodes[0] = 0;
                        octree_global. begs[0] = 0;
                        octree_global. ends[0] = points_num;

                        octree_global.parent[0] = -1;
                        //pcl::device::base::setParent(octree_global, 0, -1);
                        storage.nodes_num = 1;

                        //init tasks
                        storage.tasks_beg = 0;
                        storage.tasks_end = 1;                
                        storage.terminate = false;
                    }

                    int level = 0;

                    int  cell_begs[KernalPolicy::ARITY + 1];
                    char cell_code[KernalPolicy::ARITY];

                    __syncthreads();

                    while(storage.tasks_end - storage.tasks_beg > 0)            
                    {                     
                        int taskCount = storage.tasks_end - storage.tasks_beg;
                        int cell_count = 0;

                        if (tid < taskCount)
                        {
                            int task = storage.tasks_beg + tid;                
                            cell_count = FindCells(task, level, cell_begs, cell_code);                                
                        }

                        storage.offsets[tid] = cell_count;

                        __syncthreads();

                        //TODO scans whole block, but can only if (tid < taskCount),  
                        //optional TODO + 1 to calculate something like total_new.                
                        scan_block<pcl::device::exclusive>(storage.offsets);

                        //__syncthreads();  //because sync is inside the scan above

                        if (tid < taskCount)
                        {
                            int task = storage.tasks_beg + tid;

                            if (cell_count > 0)
                            {
                                int parent_code_shifted = octree_global.codes[task] << KernalPolicy::LEVEL_BITS_NUM;

                                int offset = storage.offsets[tid];

                                int mask = 0;
                                for(int i = 0; i < cell_count; ++i)
                                {
                                    octree_global.begs [storage.nodes_num + offset + i] = cell_begs[i];
                                    octree_global.ends [storage.nodes_num + offset + i] = cell_begs[i + 1];
                                    octree_global.codes[storage.nodes_num + offset + i] = parent_code_shifted + cell_code[i];
                                    
                                    octree_global.parent[storage.nodes_num + offset + i] = task;
                                    //pcl::device::base::setParent(octree_global, storage.nodes_num + offset + i, task);

                                    mask |= (1 << cell_code[i]);

                                }
                                octree_global.nodes[task] = ((storage.nodes_num + offset) << 8) + mask;
                            }            
                        };

                        ++level;
                        __syncthreads();

                        if (tid == taskCount - 1)
                        {
                            int total_new = cell_count + storage.offsets[tid];

                            storage.nodes_num += total_new;
                            storage.tasks_beg  = storage.tasks_end;
                            storage.tasks_end += total_new;        

                            if (total_new > blockDim.x)
                                storage.terminate = true;
                        }   

                        __syncthreads();

                        if (storage.terminate)
                            return level;
                    }
                    return 0;
                }
            };

            template<typename KernelPolicy>
            __global__ void Kernel(TasksGlobal tasks_global, OctreeGlobal octree_global, const int *codes, int points_num)
            {   
                typedef typename KernelPolicy::SmemStorage SmemStorage;

                __shared__ SmemStorage storage;                               

                Cta_InitialGlobOctree<KernelPolicy> cta(octree_global, codes, storage);
                int last_level = cta.Build(points_num);

                CopyKernel(&storage.nodes_num, octree_global.nodes_num, 1);

                GenerateTasksKernel(tasks_global.line[tasks_global.active_selector], storage.tasks_beg, storage.tasks_end, last_level);

                if (threadIdx.x == 0)
                    *tasks_global.count[tasks_global.active_selector] = storage.tasks_end - storage.tasks_beg;            
            }
        }
    }
}
#endif /* PCL_GPU_OCTREE_STA_INITIAL */
