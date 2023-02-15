/*
   Copyright, Samsung Corporation

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

 * Redistributions of source code must retain the above copyright
 notice, this list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright
 notice, this list of conditions and the following disclaimer in
 the documentation and/or other materials provided with the
 distribution.

 * Neither the name of the copyright holder nor the names of its
 contributors may be used to endorse or promote products derived
 from this software without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.  */

#include "core/include/internal/init.h"
#include "core/include/internal/config.h"
#include "internal/smalloc.h"
#include "jemalloc/jemalloc.h"
#include <sys/sysinfo.h>

/*******************************************************************************
 * Metadata statistics functions
 ******************************************************************************/

void init_node_stats() {
    FILE* fs;
    fs = fopen("/proc/zoneinfo", "r");
    if (!fs) {
        fprintf(stderr, "get zoneinfo failure\n");
        return;
    }
    int normal_zone_node=0;
    int exmem_zone_node=0;
    char str[MAX_CHAR_LEN];
    char *zone;

    while(1){
        if(fgets(str,MAX_CHAR_LEN,fs) == NULL){
            break;
        }

        if(!strncmp(str,"Node",4)){
            strtok(str," ");
            strtok(NULL,",");
            strtok(NULL," ");
            zone=strtok(NULL," \n");
            if(!strcmp(zone,NAME_NORMAL_ZONE)){
                normal_zone_node++;
            }else if(!strcmp(zone,NAME_EXMEM_ZONE)){
                exmem_zone_node++;
            }
        }
    }
    fclose(fs);

    smdk_info.stats_per_node[mem_zone_normal].nr_node = normal_zone_node;
    smdk_info.stats_per_node[mem_zone_exmem].nr_node = exmem_zone_node;
    smdk_info.stats_per_node[mem_zone_normal].total \
        = (size_t *)calloc(sizeof(size_t) , normal_zone_node);
    smdk_info.stats_per_node[mem_zone_normal].available \
        = (size_t *)calloc(sizeof(size_t) , normal_zone_node);
    smdk_info.stats_per_node[mem_zone_exmem].total \
        = (size_t *)calloc(sizeof(size_t) , exmem_zone_node);
    smdk_info.stats_per_node[mem_zone_exmem].available \
        = (size_t *)calloc(sizeof(size_t) , exmem_zone_node);
}

void init_system_mem_size(void) { //set mem_stats 'total' during initialization
    init_node_stats();
    FILE* fs;
    fs = fopen("/proc/zoneinfo", "r");
    if (!fs) {
        fprintf(stderr, "get zoneinfo failure\n");
        return;
    }

    char str[MAX_CHAR_LEN];
    char *zone;
    char *tok;
    int node;
    long page_size = sysconf(_SC_PAGESIZE);

    size_t normal_zone_total = 0;
    size_t exmem_zone_total = 0;
    while(1){
        if(fgets(str,MAX_CHAR_LEN,fs)==NULL){
            break;
        }
        if(!strcmp(strtok(str," "),"Node")){
            node = atoi(strtok(NULL,","));
            strtok(NULL," ");
            zone = strtok(NULL," \n");
            if(!strcmp(zone,NAME_NORMAL_ZONE)){
                if(fgets(str,MAX_CHAR_LEN,fs)==NULL){
                    goto parsing_fail;
                }
                tok=strtok(str," ");
                while(strcmp(tok,"present")){
                    if(fgets(str,MAX_CHAR_LEN,fs)==NULL){
                        goto parsing_fail;
                    }
                    tok=strtok(str," ");
                }
                smdk_info.stats_per_node[mem_zone_normal].total[node] = atoi(strtok(NULL," "))*page_size;
                normal_zone_total+=smdk_info.stats_per_node[mem_zone_normal].total[node];
            }else if(!strcmp(zone,NAME_EXMEM_ZONE)){
                if(fgets(str,MAX_CHAR_LEN,fs)==NULL){
                    goto parsing_fail;
                }
                tok=strtok(str," ");
                while(strcmp(tok,"present")){
                    if(fgets(str,MAX_CHAR_LEN,fs)==NULL){
                        goto parsing_fail;
                    }
                    tok=strtok(str," ");
                }
                smdk_info.stats_per_node[mem_zone_exmem].total[node]=atoi(strtok(NULL," "))*page_size;
                exmem_zone_total+=smdk_info.stats_per_node[mem_zone_exmem].total[node];
            }
        }
    }
    fclose(fs);

    smdk_info.mem_stats_perzone[mem_zone_normal].total = normal_zone_total;
    smdk_info.mem_stats_perzone[mem_zone_exmem].total = exmem_zone_total;
    return;

parsing_fail:
    fclose(fs);
    fprintf(stderr, "parsing zoneinfo failure\n");
    return;
}

size_t get_mem_stats_total(mem_zone_t zone){ //metaAPI
    if (!smdk_info.smdk_initialized){
        init_smalloc();
    }
    return smdk_info.mem_stats_perzone[zone].total;
}

size_t get_mem_stats_used(mem_zone_t zone){ //metaAPI
    char buf[100];
    int pool_idx = (opt_smdk.prio[0] == zone)? 0:1;
    uint64_t epoch = 1;
    size_t sz = sizeof(epoch);
    je_mallctl("epoch", &epoch, &sz, &epoch, sz);

    size_t memsize_used=0;
    size_t allocated=0;
    for(int i=0;i<g_arena_pool[pool_idx].nr_arena;i++){
        snprintf(buf,100,"stats.arenas.%d.small.allocated",g_arena_pool[pool_idx].arena_id[i]);
        if(je_mallctl(buf, &allocated, &sz, NULL, 0) == 0){
            memsize_used+=allocated;
        }
        snprintf(buf,100,"stats.arenas.%d.large.allocated",g_arena_pool[pool_idx].arena_id[i]);
        if(je_mallctl(buf, &allocated, &sz, NULL, 0) == 0){
            memsize_used+=allocated;
        }
    }
    size_t mem_used_node = 0;
    if(zone==mem_zone_normal){
        for (int i=0; i<sysconf(_SC_NPROCESSORS_ONLN); i++){
            mem_used_node += smdk_info.node_alloc_stat_normal[i];
        }
    } else if(zone==mem_zone_exmem){
        for (int i=0; i<sysconf(_SC_NPROCESSORS_ONLN); i++){
            mem_used_node += smdk_info.node_alloc_stat_exmem[i];
        }
    }
 
    smdk_info.mem_stats_perzone[zone].used = memsize_used + mem_used_node;
    return memsize_used + mem_used_node;
}

size_t get_mem_stats_available(mem_zone_t zone){ //metaAPI
    /*This function calculates available memory size based on '/proc/buddyinfo'*/
    if (!smdk_info.smdk_initialized){
        init_smalloc();
    }
    FILE* fs;
    char str[MAX_CHAR_LEN];
    char* zone_name;
    char* freepages;
    char* zone_name_target[2] = {NAME_NORMAL_ZONE, NAME_EXMEM_ZONE};
    size_t memsize_available_node=0;
    size_t memsize_available_total=0;
    long page_sz = sysconf(_SC_PAGESIZE);
    int node;
    int i;
    fs = fopen("/proc/buddyinfo", "r");
    if (!fs) {
        fprintf(stderr, "get buddyinfo failure\n");
        return 0;
    }
    while(1){
        i=0;
        if(fgets(str,MAX_CHAR_LEN,fs) == NULL){
            break;
        }
        str[strlen(str) -1]='\0';
        strtok(str," ");
        node = atoi(strtok(NULL,","));
        strtok(NULL," ");
        zone_name = strtok(NULL," ");
        if(!strncasecmp(zone_name, zone_name_target[zone], strlen(zone_name_target[zone]))){
            freepages = strtok(NULL," ");
            while(freepages!=NULL){
                memsize_available_node+=atoi(freepages)*(page_sz*(1<<i));
                i++;
                freepages = strtok(NULL," ");
            }
            smdk_info.stats_per_node[zone].available[node]=memsize_available_node;
            memsize_available_total+=memsize_available_node;
            memsize_available_node = 0;
        }
    }
    fclose(fs);
    smdk_info.mem_stats_perzone[zone].available = memsize_available_total;
    return memsize_available_total;
}

size_t get_mem_stats_node_total(mem_zone_t zone, int node){
    if(node>=smdk_info.stats_per_node[zone].nr_node){
        fprintf(stderr,"[Warning] requesting node '%d' is out of range\n",node);
        return 0;
    }
    return smdk_info.stats_per_node[zone].total[node];
}

size_t get_mem_stats_node_available(mem_zone_t zone, int node){
    if(node>=smdk_info.stats_per_node[zone].nr_node){
        fprintf(stderr,"[Warning] requesting node '%d' is out of range\n",node);
        return 0;
    }
    get_mem_stats_available(zone);
    return smdk_info.stats_per_node[zone].available[node];
}

void mem_stats_print(char unit) {
    double scaler = 1;
    char *unitprint="B";
    switch(unit){
    case 'g':
    case 'G':
        scaler=1024*1024*1024;
        unitprint="GB";
        break;
    case 'm':
    case 'M':
        scaler=1024*1024;
        unitprint="MB";
        break;
    case 'k':
    case 'K':
        scaler=1024;
        unitprint="KB";
        break;
    }
    
    double mem_total_normal = (double)get_mem_stats_total(mem_zone_normal) / (double)scaler;
    double mem_total_exmem = (double)get_mem_stats_total(mem_zone_exmem) / (double)scaler;
    double mem_used_normal = (double)get_mem_stats_used(mem_zone_normal) / (double)scaler;
    double mem_used_exmem = (double)get_mem_stats_used(mem_zone_exmem) / (double)scaler;
    double mem_available_normal = (double)get_mem_stats_available(mem_zone_normal) / (double)scaler;
    double mem_available_exmem = (double)get_mem_stats_available(mem_zone_exmem) / (double)scaler;

    fprintf(stderr, "SMDK Memory allocation stats:\n");
    fprintf(stderr, "%8s %17s %17s %17s\n", "Type", "Total", "Used", "Available");
    fprintf(stderr, "%8s %15.1f%2s %15.1f%2s %15.1f%2s\n", "Normal",\
            mem_total_normal,unitprint, mem_used_normal, unitprint, 
            mem_available_normal,unitprint);
    fprintf(stderr, "%8s %15.1f%2s %15.1f%2s %15.1f%2s\n", "ExMem",\
            mem_total_exmem,unitprint, mem_used_exmem, unitprint, 
            mem_available_exmem,unitprint);
    return;
}

void stats_per_node_print(char unit) {
    double scaler = 1;
    char *unitprint="B";
    switch(unit){
    case 'g':
    case 'G':
        scaler=1024*1024*1024;
        unitprint="GB";
        break;
    case 'm':
    case 'M':
        scaler=1024*1024;
        unitprint="MB";
        break;
    case 'k':
    case 'K':
        scaler=1024;
        unitprint="KB";
        break;
    }
    int i;
    get_mem_stats_available(mem_zone_normal);
    get_mem_stats_available(mem_zone_exmem);

    fprintf(stderr, "SMDK Memory allocation stats per node:\n");
    fprintf(stderr, "%8s %6s %17s %17s\n", "Type", "Node", "Total", "Available");
    if(smdk_info.stats_per_node[mem_zone_normal].nr_node){
        for(i=0;i<smdk_info.stats_per_node[mem_zone_normal].nr_node; i++){
            if(smdk_info.stats_per_node[mem_zone_normal].total[i]>0){
                fprintf(stderr, "%8s %6d %15.1f%2s %15.1f%2s\n", "Normal", i,
                        (double)smdk_info.stats_per_node[mem_zone_normal].total[i]/scaler,unitprint,
                        (double)smdk_info.stats_per_node[mem_zone_normal].available[i]/scaler,unitprint);
            }
        }
    }
    if(smdk_info.stats_per_node[mem_zone_exmem].nr_node){
        for(i=0;i<smdk_info.stats_per_node[mem_zone_exmem].nr_node; i++){
            if(smdk_info.stats_per_node[mem_zone_exmem].total[i]>0){
                fprintf(stderr, "%8s %6d %15.1f%2s %15.1f%2s\n", "ExMem", i,
                        (double)smdk_info.stats_per_node[mem_zone_exmem].total[i]/scaler,unitprint,
                        (double)smdk_info.stats_per_node[mem_zone_exmem].available[i]/scaler,unitprint);
            }
        }
    }
    return;
}
