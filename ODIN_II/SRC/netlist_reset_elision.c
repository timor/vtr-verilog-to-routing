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

void detect_and_remove_reset(netlist_t *netlist){
	printf("Reset Elision...\n");

	reset_candidate_count = 0;
	reset_candidate_node = NULL;

	check_reset_per_latch(netlist);
	if(reset_candidate_count == 0){
		return;
	}

	exclude_inputs_directly_driving_outputs(netlist);
	if(reset_candidate_count == 0){
		return;
	}

	//printf("%d potential resets left\n", reset_candidate_count);
	if(reset_candidate_count == 1){
		remove_reset(netlist, reset_candidate_node);
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
	//printf("*** Reset Elision: Checking latch-driver %s(%d)\n", node->name, node->type);

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
					if(driver_node->reset_candidate == 0){
						driver_node->reset_candidate = 1;
						reset_candidate_count++;
					}

					mark_input_as_reset(driver_node, (is_0 == node->bit_map_line_count));


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
				} else {
					//Potential reset
					if(driver_node->reset_candidate == 0){
						driver_node->reset_candidate = 1;
						reset_candidate_count++;
					}
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
		}
		input_node->potential_reset_value = 0;
		reset_candidate_node = input_node;
	} else {
		if(input_node->potential_reset_value == 0){
			//Reset values collision
			//Not a reset
			if(input_node->reset_candidate == 1){
				reset_candidate_count--;
			}
			input_node->reset_candidate = -1;
		}
		input_node->potential_reset_value = 1;
		reset_candidate_node = input_node;
	}

	//printf("Potential reset %s (to be fixed to %d)\n", input_node->name, input_node->potential_reset_value);
}

void remove_reset(netlist_t *netlist, nnode_t *reset_node){
	printf("Removing reset input %s, to be fixed to value %d\n", reset_node->name, reset_node->potential_reset_value);
}

