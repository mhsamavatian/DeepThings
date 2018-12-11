#include "ftp.h"
#include "configure.h"
#include "inference_engine_helper.h"

static inline void grid(network_parameters* net_para, ftp_parameters* ftp_para, uint32_t M, uint32_t N){
   int32_t w = net_para->output_maps[ftp_para->from_layer+ftp_para->fused_layers-1].w;
   int32_t h = net_para->output_maps[ftp_para->from_layer+ftp_para->fused_layers-1].h;
   int32_t partition_w = M;
   int32_t partition_h = N;
   int32_t stride_w = ceil(((float)w)/((float)partition_w));    
   int32_t start_w = 0;
   int32_t end_w = stride_w - 1;
   int32_t stride_h = ceil(((float)h)/((float)partition_h));    
   int32_t start_h = 0;
   int32_t end_h = stride_h - 1;
   int32_t i, j, task_id;

   for(i = 0; i < partition_h; i++){
      start_w = 0;
      end_w = stride_w - 1;	 
      for(j = 0; j < partition_w; j++){
         task_id = ftp_para->task_id[i][j];
         ftp_para->output_tiles[task_id][ftp_para->fused_layers-1].w1 = start_w;
         ftp_para->output_tiles[task_id][ftp_para->fused_layers-1].w2 = end_w;
         ftp_para->output_tiles[task_id][ftp_para->fused_layers-1].h1 = start_h;
         ftp_para->output_tiles[task_id][ftp_para->fused_layers-1].h2 = end_h;
         ftp_para->output_tiles[task_id][ftp_para->fused_layers-1].h = end_h - start_h + 1;
         ftp_para->output_tiles[task_id][ftp_para->fused_layers-1].w = end_w - start_w + 1;
         start_w = end_w + 1;
         if(j == (partition_w-2)) {end_w = w - 1;}
         else {end_w = end_w + stride_w;}
      }
      start_h = end_h + 1;
      if(i == (partition_h-2)) {end_h = h - 1;}
      else {end_h = end_h + stride_h;}
   }
}

/*
Input:
   ftp_para->output_tiles[ftp_para->task_id[i][j]][l]
Output:
   ftp_para->input_tiles[ftp_para->task_id[i][j]][l]; 
*/
static tile_region traversal(network_parameters* net_para, tile_region output, uint32_t l){
   tile_region input; 
   int32_t stride = net_para->stride[l];
   int32_t filter = net_para->filter[l];    
   int32_t w = net_para->input_maps[l].w;
   int32_t h = net_para->input_maps[l].h;     

   if(net_para->type[l] == CONV_LAYER){
      input.w1 = (output.w1*stride - filter/2)>0 ? (output.w1*stride - filter/2) : 0;
      input.w2 = (output.w2*stride + filter/2)<(w-1) ? (output.w2*stride + filter/2) : (w-1);
      input.h1 = (output.h1*stride - filter/2)>0   ? (output.h1*stride - filter/2) : 0;
      input.h2 = (output.h2*stride + filter/2)<(h-1) ? (output.h2*stride + filter/2) : (h-1);
   }else if(net_para->type[l] == POOLING_LAYER){
      input.w1 = output.w1*stride;
      input.w2 = output.w2*stride + stride -1;
      input.h1 = output.h1*stride;
      input.h2 = output.h2*stride + stride -1;
   }else { 
      fprintf(stderr, "Error: Undefined partition layer: %d\n", l);
      exit(-1);
   }
   input.w = input.w2 -input.w1 + 1;
   input.h = input.h2 -input.h1 + 1;

#if DEBUG_MULTI_FTP
   fprintf(stderr, " w1 %d, w2 %d, h1 %d, h2 %d, w %d, h %d\n", input.w1, input.w2, input.h1, input.h2, input.w, input.h);
   if (input.w < 0 || input.h < 0) {
     fprintf(stderr, "Error: negative input width/height..\n");
     exit(-1);
   }
#endif
   return input;
}

ftp_parameters* preform_ftp(uint32_t N, uint32_t M, uint32_t from_layer, uint32_t fused_layers, network_parameters* net_para){
#if DEBUG_MULTI_FTP
  fprintf(stderr, "Perform ftp from [%d, %d)\n", from_layer, from_layer + fused_layers);
#endif

   ftp_parameters* ftp_para = (ftp_parameters*)malloc(sizeof(ftp_parameters));
   ftp_para->partitions = N*M;
   ftp_para->partitions_h = N;
   ftp_para->partitions_w = M;
   ftp_para->from_layer = from_layer;
   ftp_para->fused_layers = fused_layers;

   //fprintf(stderr, "ftp_para: from_layer %d, fused_layers %d\n", from_layer, fused_layers);

   int32_t i, j, l;
   int32_t id = 0;
   for(i = 0; i < ftp_para->partitions_h; i++){
      for(j = 0; j < ftp_para->partitions_w; j++){
         ftp_para->task_id[i][j] = id;
         id++;
      }
   }
   grid(net_para, ftp_para, M, N);
   for(i = 0; i < ftp_para->partitions_h; i++){
      for(j = 0; j < ftp_para->partitions_w; j++){
#if DEBUG_MULTI_FTP
        fprintf(stderr, "FTP for task %d:\n", ftp_para->task_id[i][j]);
#endif
        // offset: from_layer
         for(l = from_layer+fused_layers-1; l >= (int32_t) from_layer; l--){
#if DEBUG_MULTI_FTP
            fprintf(stderr, " layer %d:\t", l);
#endif
            ftp_para->input_tiles[ftp_para->task_id[i][j]][l-from_layer] = 
                       traversal(net_para, ftp_para->output_tiles[ftp_para->task_id[i][j]][l-from_layer], l);
            if(l>from_layer) ftp_para->output_tiles[ftp_para->task_id[i][j]][l-from_layer-1] 
                     = ftp_para->input_tiles[ftp_para->task_id[i][j]][l-from_layer];
         }
      }
   }
   return ftp_para;
}

#if DATA_REUSE
/*Establish a dependency list, 0 means no dependencies, 1 depends on 0, 2 depends on 1 ...*/
/*For current implementation, we only have 2 levels of dependency*/
/*For example, in a 3x3 grid, the dependency is like below:       
|_0_|_1_|_0_|
|_1_|_0_|_1_|
|_0_|_1_|_0_|
, where tiles with dependency level 1 will need the overlapped data generated by tiles with level 0
*/
void reuse_aware_schedule(ftp_parameters_reuse* ftp_para_reuse){
   int32_t i, j;
   for(i = 0; i < ftp_para_reuse->partitions_h; i++){
      for(j = (i) % 2; j < ftp_para_reuse->partitions_w; j = j + 2){ 
         ftp_para_reuse->schedule[ftp_para_reuse->task_id[i][j]] = 0;
      }
   }
   for(i = 0; i < ftp_para_reuse->partitions_h; i++){
      for(j = (i + 1) % 2; j < ftp_para_reuse->partitions_w; j = j + 2){ 
         ftp_para_reuse->schedule[ftp_para_reuse->task_id[i][j]] = 1;
      }
   }
}

tile_region remove_and_record_overlapped_region_at_output(uint32_t i, uint32_t j,  uint32_t l, 
                                                     ftp_parameters_reuse* ftp_para_reuse, tile_region all_region){
   int adjacent_task;
   tile_region remaining_region = all_region;
   /*Processing the block on the left*/
   overlapped_tile_data overlapped_region;
   if(j > 0) {
      adjacent_task = ftp_para_reuse->task_id[i][j-1]; 
      remaining_region.w1 = ftp_para_reuse->output_tiles[adjacent_task][l].w2 + 1;

      overlapped_region = ftp_para_reuse->output_reuse_regions[adjacent_task][l];
      overlapped_region.right_region.w1 = all_region.w1;
      overlapped_region.right_region.w2 = ftp_para_reuse->output_tiles[adjacent_task][l].w2;
      overlapped_region.right_region.h1 = all_region.h1;
      overlapped_region.right_region.h2 = all_region.h2;
      overlapped_region.right_region.w = overlapped_region.right_region.w2 - overlapped_region.right_region.w1 + 1;
      overlapped_region.right_region.h = overlapped_region.right_region.h2 - overlapped_region.right_region.h1 + 1;
      ftp_para_reuse->output_reuse_regions[adjacent_task][l] = overlapped_region;
#if DEBUG_FTP
      //printf("---(layer %3d), left---\n", l);
      printf("---(layer %3d), left---\n", l+ftp_para_reuse+1);
      print_tile_region(overlapped_region.right_region);
#endif
   }
   /*Processing the block above*/
   if(i > 0) {
      adjacent_task = ftp_para_reuse->task_id[i-1][j]; 
      remaining_region.h1 = ftp_para_reuse->output_tiles[adjacent_task][l].h2 + 1;

      overlapped_region = ftp_para_reuse->output_reuse_regions[adjacent_task][l];
      overlapped_region.down_region.w1 = all_region.w1;
      overlapped_region.down_region.w2 = all_region.w2;
      overlapped_region.down_region.h1 = all_region.h1;
      overlapped_region.down_region.h2 = ftp_para_reuse->output_tiles[adjacent_task][l].h2;
      overlapped_region.down_region.w = overlapped_region.down_region.w2 - overlapped_region.down_region.w1 + 1;
      overlapped_region.down_region.h = overlapped_region.down_region.h2 - overlapped_region.down_region.h1 + 1;
      ftp_para_reuse->output_reuse_regions[adjacent_task][l] = overlapped_region;
#if DEBUG_FTP
      //printf("---(layer %3d), above---\n", l);
      printf("---(layer %3d), above---\n", l+ftp_para_reuse+1);
      print_tile_region(overlapped_region.down_region);
#endif
   }
   /*Processing the block on the right*/
   if((j + 1) < ftp_para_reuse->partitions_w) {
      adjacent_task = ftp_para_reuse->task_id[i][j+1]; 
      remaining_region.w2 = ftp_para_reuse->output_tiles[adjacent_task][l].w1 - 1;

      overlapped_region = ftp_para_reuse->output_reuse_regions[adjacent_task][l];
      overlapped_region.left_region.w1 = ftp_para_reuse->output_tiles[adjacent_task][l].w1;
      overlapped_region.left_region.w2 = all_region.w2;
      overlapped_region.left_region.h1 = all_region.h1;
      overlapped_region.left_region.h2 = all_region.h2;
      overlapped_region.left_region.w = overlapped_region.left_region.w2 - overlapped_region.left_region.w1 + 1;
      overlapped_region.left_region.h = overlapped_region.left_region.h2 - overlapped_region.left_region.h1 + 1;
      ftp_para_reuse->output_reuse_regions[adjacent_task][l] = overlapped_region;
#if DEBUG_FTP
      //printf("---(layer %3d), right---\n", l);
      printf("---(layer %3d), right---\n", l+ftp_para_reuse+1);
      print_tile_region(overlapped_region.left_region);
#endif
   }
   /*Processing the block below*/
   if((i + 1) < ftp_para_reuse->partitions_h) {
      adjacent_task = ftp_para_reuse->task_id[i+1][j]; 
      remaining_region.h2 = ftp_para_reuse->output_tiles[adjacent_task][l].h1 - 1;

      overlapped_region = ftp_para_reuse->output_reuse_regions[adjacent_task][l];
      overlapped_region.up_region.w1 = all_region.w1;
      overlapped_region.up_region.w2 = all_region.w2;
      overlapped_region.up_region.h1 = ftp_para_reuse->output_tiles[adjacent_task][l].h1;
      overlapped_region.up_region.h2 = all_region.h2;
      overlapped_region.up_region.w = overlapped_region.up_region.w2 - overlapped_region.up_region.w1 + 1;
      overlapped_region.up_region.h = overlapped_region.up_region.h2 - overlapped_region.up_region.h1 + 1;
      ftp_para_reuse->output_reuse_regions[adjacent_task][l] = overlapped_region;
#if DEBUG_FTP
      //printf("---(layer %3d), below---\n", l);
      printf("---(layer %3d), below---\n", l+ftp_para_reuse+1);
      print_tile_region(overlapped_region.up_region);
#endif
   }
   remaining_region.w = remaining_region.w2 - remaining_region.w1 + 1;
   remaining_region.h = remaining_region.h2 - remaining_region.h1 + 1;

  if (remaining_region.w <= 0) {
    // when the fused layer is deep, it is possible that adjacent tasks overlap
#if DEBUG_MULTI_FTP
    fprintf(stderr, "Warning: too deep fused layers causing fully overlap w...\n");
#endif
    remaining_region.w2 = remaining_region.w1; 
    remaining_region.w = 1;
  }
  if (remaining_region.h <= 0) {
#if DEBUG_MULTI_FTP
     fprintf(stderr, "Warning: too deep fused layers causing fully overlap h...\n");
#endif
    remaining_region.h2 = remaining_region.h1; 
    remaining_region.h = 1;
  }

   return remaining_region;
}


void calculate_reuse_data_size(ftp_parameters_reuse* ftp_para_reuse, network_parameters* net_para, uint32_t task_id){

   uint32_t i = task_id/(ftp_para_reuse->partitions_w);
   uint32_t j = task_id%(ftp_para_reuse->partitions_w);
   int32_t adjacent_id[4];
   uint32_t position;
   uint32_t l;
   overlapped_tile_data regions_and_data;
   tile_region overlap_index;
   for(position = 0; position < 4; position++){
      adjacent_id[position] = -1;
   }

   /*position encoding
         2
         |
   3 <- self -> 1
         |
         0
   */

   /*get the up overlapped data from tile below*/
   if((i+1)<(ftp_para_reuse->partitions_h)) adjacent_id[0] = ftp_para_reuse->task_id[i+1][j];
   /*get the left overlapped data from tile on the right*/
   if((j+1)<(ftp_para_reuse->partitions_w)) adjacent_id[1] = ftp_para_reuse->task_id[i][j+1];
   /*get the bottom overlapped data from tile above*/
   if(i>0) adjacent_id[2] = ftp_para_reuse->task_id[i-1][j];
   /*get the right overlapped data from tile on the left*/
   if(j>0) adjacent_id[3] = ftp_para_reuse->task_id[i][j-1];

   ftp_para_reuse->adjacent_reuse_data_size[task_id]=0;
   ftp_para_reuse->self_reuse_data_size[task_id]=0;

   for(l = 0; l < ftp_para_reuse->fused_layers-1; l++){
      for(position = 0; position < 4; position++){
         if(adjacent_id[position]==-1) continue;
         uint32_t mirror_position = (position + 2)%4;
         regions_and_data = ftp_para_reuse->output_reuse_regions[adjacent_id[position]][l];
         overlap_index = get_region(&regions_and_data, mirror_position);
         if((overlap_index.w>0)&&(overlap_index.h>0))
            ftp_para_reuse->adjacent_reuse_data_size[task_id] += sizeof(float)*overlap_index.w*overlap_index.h*net_para->output_maps[l+ftp_para_reuse->from_layer].c;
      }
   }

   for(l = 0; l < ftp_para_reuse->fused_layers-1; l++){
      for(position = 0; position < 4; position++){
         if(adjacent_id[position]==-1) continue;
         regions_and_data = ftp_para_reuse->output_reuse_regions[task_id][l];
         overlap_index = get_region(&regions_and_data, position);
         if((overlap_index.w>0)&&(overlap_index.h>0))
            ftp_para_reuse->self_reuse_data_size[task_id] += sizeof(float)*overlap_index.w*overlap_index.h*net_para->output_maps[l+ftp_para_reuse->from_layer].c;
      }
   }
#if DEBUG_FTP
   printf("adjacent_reuse_data_size for task %d: %u\n", task_id, ftp_para_reuse->adjacent_reuse_data_size[task_id]);
   printf("self_reuse_data_size for task %d: %u\n", task_id, ftp_para_reuse->self_reuse_data_size[task_id]);
#endif
}


/*This function must be called after perform_ftp()*/
ftp_parameters_reuse* preform_ftp_reuse(network_parameters* net_para, ftp_parameters* ftp_para){
   int32_t i, j, l;
   uint32_t task;

   ftp_parameters_reuse* ftp_para_reuse = (ftp_parameters_reuse*)malloc(sizeof(ftp_parameters_reuse));
   ftp_para_reuse->partitions = ftp_para->partitions;
   ftp_para_reuse->partitions_h = ftp_para->partitions_h;
   ftp_para_reuse->partitions_w = ftp_para->partitions_w;
   ftp_para_reuse->from_layer = ftp_para->from_layer;
   ftp_para_reuse->fused_layers = ftp_para->fused_layers;
   for(i = 0; i < ftp_para_reuse->partitions_h; i++){
      for(j = 0; j < ftp_para_reuse->partitions_w; j++){
         ftp_para_reuse->task_id[i][j] = ftp_para->task_id[i][j];
      }
   }
   reuse_aware_schedule(ftp_para_reuse);

   /*Copy the grid output from normal ftp*/
   for(i = 0; i < ftp_para_reuse->partitions_h; i++){
      for(j = 0; j < ftp_para_reuse->partitions_w; j++){
         task = ftp_para_reuse->task_id[i][j];
         l = ftp_para_reuse->fused_layers-1;
         ftp_para_reuse->output_tiles[task][l] = ftp_para->output_tiles[task][l];
      }
   }

   /*Calculate the tile regions with no reuse data dependency*/
   for(i = 0; i < ftp_para_reuse->partitions_h; i++){
      for(j = 0; j < ftp_para_reuse->partitions_w; j++){
         task = ftp_para_reuse->task_id[i][j];
         for(l = ftp_para_reuse->fused_layers-1; l >= 0; l--){
            if(ftp_para_reuse->schedule[task] == 0){
               /*If there is no dependency, just copy from normal ftp parameters*/
               ftp_para_reuse->input_tiles[task][l] = ftp_para->input_tiles[task][l];
               ftp_para_reuse->output_tiles[task][l] = ftp_para->output_tiles[task][l];
            }
         }
      }
   }

   /*Calculate the tile regions with reuse data dependency*/
   for(i = 0; i < ftp_para_reuse->partitions_h; i++){
      for(j = 0; j < ftp_para_reuse->partitions_w; j++){
         task = ftp_para_reuse->task_id[i][j];
         for(l = ftp_para_reuse->fused_layers-1; l >= 0; l--){
            ftp_para_reuse->output_reuse_regions[task][l].down_size = 0;
            ftp_para_reuse->output_reuse_regions[task][l].right_size = 0;
            ftp_para_reuse->output_reuse_regions[task][l].up_size = 0;
            ftp_para_reuse->output_reuse_regions[task][l].left_size = 0;
            ftp_para_reuse->output_reuse_regions[task][l].right_region.h = 0;
            ftp_para_reuse->output_reuse_regions[task][l].right_region.w = 0;
            ftp_para_reuse->output_reuse_regions[task][l].down_region.h = 0;
            ftp_para_reuse->output_reuse_regions[task][l].down_region.w = 0;
            ftp_para_reuse->output_reuse_regions[task][l].left_region.h = 0;
            ftp_para_reuse->output_reuse_regions[task][l].left_region.w = 0;
            ftp_para_reuse->output_reuse_regions[task][l].up_region.h = 0;
            ftp_para_reuse->output_reuse_regions[task][l].up_region.w = 0;
         }
      }
   }
   for(i = 0; i < ftp_para_reuse->partitions_h; i++){
      for(j = 0; j < ftp_para_reuse->partitions_w; j++){
#if DEBUG_FTP
         printf("----(%3d,%3d)----\n", i, j);
#endif
        task = ftp_para_reuse->task_id[i][j];
#if DEBUG_MULTI_FTP
        if(ftp_para_reuse->schedule[task] == 1)
          fprintf(stderr, "FTP_REUSE for task %d:\n", task);
#endif

         // offset: from_layer
         for(l = ftp_para_reuse->from_layer+ftp_para_reuse->fused_layers-1; l >= (int32_t) ftp_para_reuse->from_layer; l--){
            if(ftp_para_reuse->schedule[task] == 1){
#if DEBUG_MULTI_FTP
              fprintf(stderr, " layer %d:\t", l);
#endif
               ftp_para_reuse->input_tiles[ftp_para_reuse->task_id[i][j]][l-ftp_para_reuse->from_layer] = 
                       traversal(net_para, ftp_para_reuse->output_tiles[ftp_para_reuse->task_id[i][j]][l-ftp_para_reuse->from_layer], l);
               if(l>ftp_para_reuse->from_layer) 
                 ftp_para_reuse->output_tiles[ftp_para_reuse->task_id[i][j]][l-ftp_para_reuse->from_layer-1] = remove_and_record_overlapped_region_at_output(i, j, l-ftp_para_reuse->from_layer-1,  ftp_para_reuse, ftp_para_reuse->input_tiles[ftp_para_reuse->task_id[i][j]][l-ftp_para_reuse->from_layer]);
            }
#if DEBUG_FTP
            printf("---(layer %3d)---\n", l);
            print_tile_region(ftp_para_reuse->output_tiles[ftp_para->task_id[i][j]][l-ftp_para_reuse->from_layer]);
            print_tile_region(ftp_para_reuse->input_tiles[ftp_para->task_id[i][j]][l-ftp_para_reuse->from_layer]);
            printf("---(layer %3d)---\n", l);
#endif
         }
      }
   }

   for(i = 0; i < ftp_para_reuse->partitions_h; i++){
      for(j = 0; j < ftp_para_reuse->partitions_w; j++){
         task = ftp_para_reuse->task_id[i][j];
         calculate_reuse_data_size(ftp_para_reuse, net_para, task);/*Will be used in reuse_data serialization*/
      }
   }

   return ftp_para_reuse;
}

void set_coverage(ftp_parameters_reuse* ftp_para_reuse, uint32_t task_id){
   ftp_para_reuse->coverage[task_id]=1;
}

void set_missing(ftp_parameters_reuse* ftp_para_reuse, uint32_t task_id){
   ftp_para_reuse->missing[task_id]=1;
}

uint32_t get_missing(ftp_parameters_reuse* ftp_para_reuse, uint32_t task_id){
   return ftp_para_reuse->missing[task_id];
}

uint32_t get_coverage(ftp_parameters_reuse* ftp_para_reuse, uint32_t task_id){
   return ftp_para_reuse->coverage[task_id];
}

void clean_coverage(ftp_parameters_reuse* ftp_para_reuse){
   uint32_t task;
   uint32_t i, j;
   for(i = 0; i < ftp_para_reuse->partitions_h; i++){
      for(j = 0; j < ftp_para_reuse->partitions_w; j++){
         task = ftp_para_reuse->task_id[i][j];
         ftp_para_reuse->coverage[task]=0;
         ftp_para_reuse->missing[task]=0;
      }
   }
}


bool is_reuse_ready(ftp_parameters_reuse* ftp_para_reuse, uint32_t task_id){
   uint32_t i = task_id/(ftp_para_reuse->partitions_w);
   uint32_t j = task_id%(ftp_para_reuse->partitions_w);
   uint32_t adj_task;
   bool ready = true;
   if(i + 1 < ftp_para_reuse->partitions_h){
      adj_task = ftp_para_reuse->task_id[i+1][j];
      if(ftp_para_reuse->coverage[adj_task] == 0) {
         ready = false;
         return ready;
      }	
   }
   if(j + 1 < ftp_para_reuse->partitions_w){
      adj_task = ftp_para_reuse->task_id[i][j+1];
      if(ftp_para_reuse->coverage[adj_task] == 0) {
         ready = false;
         return ready;
      }	
   }
   if(j > 0){
      adj_task = ftp_para_reuse->task_id[i][j-1];
      if(ftp_para_reuse->coverage[adj_task] == 0) {
         ready = false;
         return ready;
      }	
   }
   if(i > 0){
      adj_task = ftp_para_reuse->task_id[i-1][j];
      if(ftp_para_reuse->coverage[adj_task] == 0) {
         ready = false;
         return ready;
      }	
   }
   return ready;
}


/*position encoding
         2
         |
   3 <- self -> 1
         |
         0
*/

tile_region get_region(overlapped_tile_data * overlap, uint32_t pos){
   if(pos == 0) return overlap->down_region;
   if(pos == 1) return overlap->right_region;
   if(pos == 2) return overlap->up_region;
   if(pos == 3) return overlap->left_region;
   tile_region empty;
   return empty;
}

uint32_t get_size(overlapped_tile_data * overlap, uint32_t pos){
   if(pos == 0) return overlap->down_size;
   if(pos == 1) return overlap->right_size;
   if(pos == 2) return overlap->up_size;
   if(pos == 3) return overlap->left_size;
   return 0;
}

float* get_data(overlapped_tile_data * overlap, uint32_t pos){
   if(pos == 0) return overlap->down;
   if(pos == 1) return overlap->right;
   if(pos == 2) return overlap->up;
   if(pos == 3) return overlap->left;
   return NULL;
}

void set_region(overlapped_tile_data * overlap, uint32_t pos, tile_region tile){
   if(pos == 0) overlap->down_region = tile;
   if(pos == 1) overlap->right_region = tile;
   if(pos == 2) overlap->up_region = tile;
   if(pos == 3) overlap->left_region = tile;
}

void set_size(overlapped_tile_data * overlap, uint32_t pos, uint32_t size){
   if(pos == 0) overlap->down_size = size;
   if(pos == 1) overlap->right_size = size;
   if(pos == 2) overlap->up_size = size;
   if(pos == 3) overlap->left_size = size;
}

void set_data(overlapped_tile_data * overlap, uint32_t pos, float* data){
   if(pos == 0) overlap->down = data;
   if(pos == 1) overlap->right = data;
   if(pos == 2) overlap->up = data;
   if(pos == 3) overlap->left = data;
}
#endif /*DATA_REUSE*/
void print_tile_region(tile_region tile){
   printf("tile size is (%3d,%3d) \n", tile.w, tile.h);
   printf("(%3d,%3d)--------|\n", tile.w1, tile.h1);
   printf("|----------------|\n");
   printf("|----------------|\n");
   printf("|--------(%3d,%3d)\n", tile.w2, tile.h2);
}

