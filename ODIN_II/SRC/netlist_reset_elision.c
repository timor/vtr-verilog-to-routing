/*
Permission is hereby granted, free of charge, to any person
obtaining a copy of this software and associated documentation
files (the "Software"), to deal in the Software without
restriction, including without limitation the rights to use,
copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following
conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "types.h"
#include "globals.h"
#include "errors.h"
#include "netlist_utils.h"
#include "util.h"
#include "netlist_reset_elision.h"

int _visited_output_to_inputs, _visited_check_latches;
#define VISITED_OUTPUT_TO_INPUTS ((void*)&_visited_output_to_inputs)
#define VISITED_CHECK_LATCHES ((void*)&_visited_check_latches)

int reset_candidate_count;
nnode_t *reset_candidate_node;

//node->reset_candidate: 0 don't know yet, -1 sure not a reset, 1 reset candidate

void detect_and_remove_reset(netlist_t *netlist, FILE* file){
	printf("Reset Elision...\n");

	reset_candidate_count = 0;
	reset_candidate_node = NULL;

	check_reset_per_latch(netlist);
	if(reset_candidate_count == 0){
		printf("No resets found!\n");
		return;
	}
	printf("%d potential reset(s) discovered\n", reset_candidate_count);

	exclude_inputs_directly_driving_outputs(netlist);
	if(reset_candidate_count == 0){
		printf("All reset candidates are directly connected to wire outputs\n");
		return;
	}

	if(reset_candidate_count == 1){
		printf("Outputting the no-reset netlist\n");
		//remove_reset(netlist, reset_candidate_node);
		print_remove_reset(netlist, reset_candidate_node, file);
	}
}

void exclude_inputs_directly_driving_outputs(netlist_t *netlist){
	int i;
	for(i = 0; i < netlist->num_top_output_nodes; i++){
		traverse_from_outputs(netlist->top_output_nodes[i]);
	}
}

void traverse_from_outputs(nnode_t *node){
	if(node->node_data == VISITED_OUTPUT_TO_INPUTS) return; // Already visited
	node->node_data = VISITED_OUTPUT_TO_INPUTS; // Mark as visited

	if(node->type == FF_NODE || node->type == MEMORY){
		return;
	}

	if(node->type == INPUT_NODE){
		if(node->reset_candidate == 1){
			reset_candidate_count--;
		}
		node->reset_candidate = -1;
		printf("%s not a reset!\n", node->name);
		return;
	}

	int i;
	for(i = 0; i < node->num_input_pins; i++){
		if(node->input_pins[i]->net->driver_pin){ // ensure this net has a driver (i.e. skip undriven outputs)
			traverse_from_outputs(node->input_pins[i]->net->driver_pin->node); // Visit the drivers of this node
		}
	}
}

void check_reset_per_latch(netlist_t *netlist){
	int i;
	for(i = 0; i < netlist->num_top_output_nodes; i++){
		traverse_check_reset_per_latch(netlist->top_output_nodes[i]);
	}
}

void traverse_check_reset_per_latch(nnode_t *node){
	if(node->node_data == VISITED_CHECK_LATCHES) return; // Already visited
	node->node_data = VISITED_CHECK_LATCHES; // Mark as visited

	//printf("*** Reset Elision: Traversing node %s(%d)\n", node->name, node->type);

	if(node->type == FF_NODE){
		check_latch_driver(node->input_pins[0]->net->driver_pin->node, node);
	}

	int i;
	for(i = 0; i < node->num_input_pins; i++){
		if(node->input_pins[i]->net->driver_pin){ // ensure this net has a driver (i.e. skip undriven outputs)
			traverse_check_reset_per_latch(node->input_pins[i]->net->driver_pin->node); // Visit the drivers of this node
		}
	}
}

void check_latch_driver(nnode_t *node, nnode_t *latch_node){
	printf("*** Reset Elision: Checking latch-driver %s(%d)\n", node->name, node->type);

	int i;
	for(i = 0; i < node->num_input_pins; i++){
		if(node->input_pins[i]->net->driver_pin){ // ensure this net has a driver (i.e. skip undriven outputs)
			nnode_t *driver_node = node->input_pins[i]->net->driver_pin->node;
			if(driver_node->type == INPUT_NODE && driver_node->reset_candidate != -1){
				//Ensure this input resets this latch:

				//Count bits of driving input
				int is_0 = 0;
				int is_1 = 0;
				int is_dash = 0;
				int last0 = -1;
				int last1 = -1;

				int j;
				for(j = 0; j < node->bit_map_line_count; j++){
					switch(node->bit_map[j][i]){
					case '0':
						is_0++;
						last0 = j;
						break;
					case '1':
						is_1++;
						last1 = j;
						break;
					case '-':
						is_dash++;
						break;
					}
				}

				//CASE 1: Either all lines contain the same bit
				if (is_0 == node->bit_map_line_count || is_1 == node->bit_map_line_count){
					//Potential reset
					mark_input_as_reset(driver_node, (node->bit_map[0][i] == '1'));


					if(node->is_on_gate){
						latch_node->derived_initial_value = 0;
					} else {
						latch_node->derived_initial_value = 1;
					}

					//printf("Latch %s init value: %d\n", latch_node->name, latch_node->derived_initial_value);

					break;
				}

				//CASE 2: One line contains one bit and all other drivers are -
				//        also, all other lines the opposite bit
				int case2reset = 1;
				if ((is_0 == 1 && is_1 == node->bit_map_line_count - 1)){
					//Check if all other drivers on this line are -
					for(j = 0; j < node->num_input_pins; j++){
						if(j != i){
							//printf("[%d][%d] = %c\n", j, i, node->bit_map[last0][j]);
							if(node->bit_map[last0][j] != '-'){
								case2reset = 0;
								break;
							}
						}
					}
				}

				//Special case needed to distinguish two lines
				if(node->bit_map_line_count == 2){
					if(case2reset == 0){
						case2reset = 2;
					}
				} else {
					if(case2reset == 1){
						case2reset = 2;
					}
				}

				if(case2reset!=0 && is_1 == 1 && is_0 == node->bit_map_line_count - 1){
					//Check if all other drivers are -
					for(j = 0; j < node->bit_map_line_count; j++){
						if(j != i){
							//printf("[%d][%d] = %c\n", j, i, node->bit_map[last1][j]);
							if(node->bit_map[last1][j] != '-'){
								case2reset = 0;
								break;
							}
						}
					}
				}

				if(!case2reset){
					//Not a reset
					if(driver_node->reset_candidate == 1){
						reset_candidate_count--;
					}
					driver_node->reset_candidate = -1;
					printf("%s not a reset!\n", driver_node->name);
				} else {
					//Potential reset
					mark_input_as_reset(driver_node, (case2reset == 2));

					if(node->is_on_gate){
						latch_node->derived_initial_value = 1;
					} else {
						latch_node->derived_initial_value = 0;
					}

					//printf("Latch %s init value: %d\n", latch_node->name, latch_node->derived_initial_value);
				}
			}
		}
	}
}

void mark_input_as_reset(nnode_t *input_node, int is_positive_reset){

	if(is_positive_reset){
		if(input_node->potential_reset_value == 1){
			//Reset values collision
			//Not a reset
			if(input_node->reset_candidate == 1){
				reset_candidate_count--;
			}
			input_node->reset_candidate = -1;
			printf("%s not a reset!\n", input_node->name);
		} else {
			if(input_node->reset_candidate == 0){
				reset_candidate_count++;
			}
			input_node->reset_candidate = 1;
			printf("%s may be a reset!\n", input_node->name);

			input_node->potential_reset_value = 0;
			reset_candidate_node = input_node;
		}
	} else {
		if(input_node->potential_reset_value == 0){
			//Reset values collision
			//Not a reset
			if(input_node->reset_candidate == 1){
				reset_candidate_count--;
			}
			input_node->reset_candidate = -1;
			printf("%s not a reset!\n", input_node->name);
		} else {
			if(input_node->reset_candidate == 0){
				reset_candidate_count++;
			}
			input_node->reset_candidate = 1;
			input_node->potential_reset_value = 1;
			printf("%s may be a reset!\n", input_node->name);

			reset_candidate_node = input_node;
		}
	}

	//printf("Potential reset %s (to be fixed to %d)\n", input_node->name, input_node->potential_reset_value);
}

void remove_reset(netlist_t *netlist, nnode_t *reset_node){
	printf("Removing reset input %s, to be fixed to value %d\n", reset_node->name, reset_node->potential_reset_value);

	/*Connect pins of children of reset to either gnd or vcc*/
	nnode_t* new_driver_node = (reset_node->potential_reset_value == 1? netlist->gnd_node: netlist->vcc_node);

	int i, j;
	int num_children = 0;
	nnode_t **children = get_children_of(reset_node, &num_children);
	for (i = 0; i < num_children; i++){

		nnode_t* lut_node = children[i];

		int num_grand_children = 0;
		nnode_t **grand_children = get_children_of(children[i], &num_grand_children);
		for (j = 0; j < num_grand_children; j++){
			nnode_t* node = grand_children[j];

			if(node->type == FF_NODE){
				/*Set initial values for FF_nodes*/
				node->has_initial_value = 1;
				node->initial_value = node->derived_initial_value;
			}
		}

		for(j = 0; j < lut_node->num_input_pins; j++){
			if(lut_node->input_pins[j]->net->driver_pin->node == reset_node){
				//printf("old_node: %s\n", lut_node->input_pins[j]->net->driver_pin->node->name);
				//printf("remap_pin_to_new_node: %s %s\n", lut_node->input_pins[j]->name, new_driver_node->name);
				remap_pin_to_new_net(lut_node->input_pins[j], new_driver_node->output_pins[0]->net);
				//printf("new_node: %s\n", lut_node->input_pins[j]->net->driver_pin->node->name);
			}
		}
	}
	free(children);
}

void print_remove_reset(netlist_t *netlist, nnode_t *reset_node, FILE* file){
	printf("Creating new BLIF file to output %s\n", global_args.output_file);
	//printf("Removing reset input %s, to be fixed to value %d\n", reset_node->name, reset_node->potential_reset_value);

	/*Connect pins of children of reset to either gnd or vcc*/
	nnode_t* new_driver_node = (reset_node->potential_reset_value == 1? netlist->gnd_node: netlist->vcc_node);

	//Create output file
	FILE* out = fopen(global_args.output_file, "w");
	if (out == NULL){
		error_message(NETLIST_ERROR, -1, -1, "Could not open output file %s\n", global_args.output_file);
	}

	//Copy input to output
	rewind(file);
	char line[4096];
	char to_replace[128];
	int first = 1;
	while(fgets(line, 4096, file)){

		//Rename all gate references of reset to either GND or VCC
		if(strstr(line, ".names") && strstr(line, reset_node->name)){
			printf("replacing line: %s", line);
			if(first){
				//Print VCC or GND the first time
				first = 0;
				if(reset_node->potential_reset_value == 1){
					fprintf(out, ".names vcc\n1\n\n");
					strncpy(to_replace, "vcc", 4);
				} else {
					fprintf(out, ".names gnd\n\n");
					strncpy(to_replace, "gnd", 4);
				}
			}

			string_replace(line, reset_node->name, to_replace);
		}
		fprintf(out, "%s", line);
	}

	fclose(out);


	int i, j;
	int num_children = 0;
	nnode_t **children = get_children_of(reset_node, &num_children);
	for (i = 0; i < num_children; i++){

		nnode_t* lut_node = children[i];

		int num_grand_children = 0;
		nnode_t **grand_children = get_children_of(children[i], &num_grand_children);
		for (j = 0; j < num_grand_children; j++){
			nnode_t* node = grand_children[j];

			if(node->type == FF_NODE){
				/*Set initial values for FF_nodes*/
				node->has_initial_value = 1;
				node->initial_value = node->derived_initial_value;
			}
		}

		for(j = 0; j < lut_node->num_input_pins; j++){
			if(lut_node->input_pins[j]->net->driver_pin->node == reset_node){
				//printf("old_node: %s\n", lut_node->input_pins[j]->net->driver_pin->node->name);
				//printf("remap_pin_to_new_node: %s %s\n", lut_node->input_pins[j]->name, new_driver_node->name);
				remap_pin_to_new_net(lut_node->input_pins[j], new_driver_node->output_pins[0]->net);
				//printf("new_node: %s\n", lut_node->input_pins[j]->net->driver_pin->node->name);
			}
		}
	}
	free(children);
}

void string_replace(char *line, char *old_word, char *new_word){
	char new_line[4096];
	char* pos;

	memset(new_line, 0, 4096);

	pos = strstr(line, old_word);
	memcpy(new_line, line, (unsigned long long)pos - (unsigned long long)line);
	new_line[(unsigned long long)pos - (unsigned long long)line + 1] = 0;

	strcat(new_line, new_word);
	strcat(new_line, pos + strlen(old_word));

	printf("%s", new_line);

	strcpy(line, new_line);
}
