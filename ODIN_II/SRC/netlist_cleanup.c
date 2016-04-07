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


/* Used in the nnode_t.node_data field to mark if the node was already visited
 * during a forward or backward sweep traversal or the removal phase */
int _visited_forward, _visited_backward, _visited_removal, _visited_reset;
#define VISITED_FORWARD ((void*)&_visited_forward)
#define VISITED_BACKWARD ((void*)&_visited_backward)
#define VISITED_REMOVAL ((void*)&_visited_removal)
#define VISITED_RESET ((void*)&_visited_reset)


/* Simple linked list of nodes structure */
typedef struct node_list_t_t{
	nnode_t *node;
	struct node_list_t_t *next;
} node_list_t;

node_list_t* insert_node_list(node_list_t* node_list, nnode_t *node){
	node_list->node = node;
	node_list->next = (node_list_t*)calloc(1, sizeof(node_list_t));
	return node_list->next;
}

node_list_t useless_nodes; // List of the nodes to be removed
node_list_t *removal_list_next = &useless_nodes; // Tail of the nodes to be removed

node_list_t addsub_nodes; // List of the adder/subtractor nodes
node_list_t *addsub_list_next = &addsub_nodes; // Tail of the adder/subtractor node list



/* Traverse the netlist backwards, moving from outputs to inputs */
void traverse_backward(nnode_t *node){
	if(node->node_data == VISITED_BACKWARD) return; // Already visited
	node->node_data = VISITED_BACKWARD; // Mark as visited
	int i;
	for(i = 0; i < node->num_input_pins; i++){
		if(node->input_pins[i]->net->driver_pin){ // ensure this net has a driver (i.e. skip undriven outputs)
			traverse_backward(node->input_pins[i]->net->driver_pin->node); // Visit the drivers of this node
		}
	}
}

/* Traverse the netlist forward, moving from inputs to outputs.
 * Adds nodes that do not affect any outputs to the useless_nodes list 
 * Arguments:
 * 	node: the current node in the netlist
 * 	toplevel: are we at one of the top-level nodes? (GND, VCC, PAD or INPUT)
 * 	remove_me: should the current node be removed?
 * */
void traverse_forward(nnode_t *node, int toplevel, int remove_me){
	if(node == NULL) return; // Shouldn't happen, but check just in case
	if(node->node_data == VISITED_FORWARD) return; // Already visited, shouldn't happen anyway
	
	/* We want to remove this node if either its parent was removed, 
	 * or if it was not visited on the backwards sweep */
	remove_me = remove_me || ((node->node_data != VISITED_BACKWARD) && (toplevel == FALSE));
	
	/* Mark this node as visited */
	node->node_data = VISITED_FORWARD;
	
	if(remove_me) {
		/* Add this node to the list of nodes to remove */
		removal_list_next = insert_node_list(removal_list_next, node);
	}
	
	if(node->type == ADD || node->type == MINUS){
		/* Check if we've found the head of an adder or subtractor chain */
		if(node->input_pins[node->num_input_pins-1]->net->driver_pin->node->type == PAD_NODE){
			addsub_list_next = insert_node_list(addsub_list_next, node);
		}
	}
	
	/* Iterate through every fanout node */
	int i, j;
	for(i = 0; i < node->num_output_pins; i++){
		if(node->output_pins[i] && node->output_pins[i]->net){
			for(j = 0; j < node->output_pins[i]->net->num_fanout_pins; j++){
				if(node->output_pins[i]->net->fanout_pins[j]){
					nnode_t *child = node->output_pins[i]->net->fanout_pins[j]->node;
					if(child){
						/* If this child hasn't already been visited, visit it now */
						if(child->node_data != VISITED_FORWARD){
							traverse_forward(child, FALSE, remove_me);
						}
					}
				}
			}
		}
	}
}


/* Start at each of the top level output nodes and traverse backwards to the inputs
to determine which nodes have an effect on the outputs */
void mark_output_dependencies(netlist_t *netlist){
	int i;
	for(i = 0; i < netlist->num_top_output_nodes; i++){
		traverse_backward(netlist->top_output_nodes[i]);
	}
}

/* Traversed the netlist forward from the top level inputs and special nodes 
(VCC, GND, PAD) */
void identify_unused_nodes(netlist_t *netlist){

	useless_nodes.node = NULL;
	useless_nodes.next = NULL;
	
	addsub_nodes.node = NULL;
	addsub_nodes.next = NULL;
		
	traverse_forward(netlist->gnd_node, TRUE, FALSE);
	traverse_forward(netlist->vcc_node, TRUE, FALSE);
	traverse_forward(netlist->pad_node, TRUE, FALSE);
	int i;
	for(i = 0; i < netlist->num_top_input_nodes; i++){
		traverse_forward(netlist->top_input_nodes[i], TRUE, FALSE);
	}
}

/* Note: This does not actually free the unused logic, but simply detaches
it from the rest of the circuit */
void remove_unused_nodes(node_list_t *remove){
	while(remove != NULL && remove->node != NULL){
		int i;		
		for(i = 0; i < remove->node->num_input_pins; i++){
			npin_t *input_pin = remove->node->input_pins[i];
			input_pin->net->fanout_pins[input_pin->pin_net_idx] = NULL; // Remove the fanout pin from the net
		}
		remove->node->node_data = VISITED_REMOVAL;
		remove = remove->next;
	}
}

/* Since we are traversing the entire netlist anyway, we can use this
 * opportunity to keep track of the heads of adder/subtractors chains
 * and then compute statistics on them */
int adder_chain_count = 0;
int longest_adder_chain = 0;
int total_adders = 0;

int subtractor_chain_count = 0;
int longest_subtractor_chain = 0;
int total_subtractors = 0;

double geomean_addsub_length = 0.0; // Geometric mean of add/sub chain length
double sum_of_addsub_logs = 0.0; // Sum of the logarithms of the add/sub chain lengths; used for geomean
int total_addsub_chain_count = 0;

void calculate_addsub_statistics(node_list_t *addsub){
	while(addsub != NULL && addsub->node != NULL){
		int found_tail = FALSE;
		nnode_t *node = addsub->node;
		int chain_depth = 0;
		while(!found_tail){
			if(node->node_data == VISITED_REMOVAL){
				found_tail = TRUE;
				break;
			}
			chain_depth += 1;
			
			/* Carry out is always output pin 0 */
			nnet_t *carry_out_net = node->output_pins[0]->net;
			if(carry_out_net == NULL || carry_out_net->fanout_pins[0] == NULL) found_tail = TRUE;
			else node = carry_out_net->fanout_pins[0]->node;
		}
		if(chain_depth > 0){
			if(node->type == ADD){
				adder_chain_count += 1;
				total_adders += chain_depth;
				if(chain_depth > longest_adder_chain) longest_adder_chain = chain_depth;
			}
			else if(node->type == MINUS){
				subtractor_chain_count += 1;
				total_subtractors += chain_depth;
				if(chain_depth > longest_subtractor_chain) longest_subtractor_chain = chain_depth;
			}
			
			sum_of_addsub_logs += log(chain_depth);
			total_addsub_chain_count++;
		}
		
		addsub = addsub->next;
	}
	/* Calculate the geometric mean carry chain length */
	geomean_addsub_length = exp(sum_of_addsub_logs / total_addsub_chain_count);
}

/*
 * *********************
 * Panos Reset Elision *
 * *********************
 */

int simulate_for_reset(netlist_t *netlist, nnode_t* potential_rst, int cycle, signed char rst_value)
{
	//printf("******* Simulating for potential reset %s, value=%d, cycle=%d:\n", potential_rst->name, rst_value, cycle);
	int reset_candidate = -1;

	queue_t *queue = create_queue();
	int i;

	for (i = 0; i < netlist->num_top_input_nodes; i++){
		enqueue_node_if_ready(queue,netlist->top_input_nodes[i],cycle);
		update_pin_value(netlist->top_input_nodes[i]->output_pins[0], -1, cycle);
	}

	update_pin_value(potential_rst->output_pins[0], rst_value, cycle);

	// Enqueue constant nodes.
	nnode_t *constant_nodes[] = {netlist->gnd_node, netlist->vcc_node, netlist->pad_node};
	int num_constant_nodes = 3;
	for (i = 0; i < num_constant_nodes; i++)
		enqueue_node_if_ready(queue,constant_nodes[i],cycle);

	nnode_t *node;
	while ((node = (nnode_t *)queue->remove(queue)))
	{
		compute_and_store_value(node, cycle);
		//printf("***** %s (%d) %d\n", node->name, node->type, get_pin_value(node->output_pins[0], cycle));

		if(node->type==FF_NODE){
			//printf("***** %s (%d) %d\n", node->name, node->type, get_pin_value(node->output_pins[0], cycle));

			signed char latch_value = get_pin_value(node->output_pins[0], cycle);


			if(cycle == 0 && reset_candidate != 0){
				if (latch_value != -1){
					reset_candidate = 0;
				} else {
					//printf("***** %s (%d) %d\n", node->name, node->type, get_pin_value(node->output_pins[0], cycle));
					reset_candidate = 1;
				}
			}

			if(cycle == 1 && reset_candidate != 0) {
				if(get_pin_value(node->output_pins[0], 0) == -1 && latch_value != -1){
					//printf("***** %s (%d) %d\n", node->name, node->type, get_pin_value(node->output_pins[0], cycle));
					reset_candidate = 1;
				}
			}

			/*printf("***** %s:\t", node->name);
			for(i=0; i < node->num_output_pins; i++){
				printf("%d(%d) ", get_pin_value(node->output_pins[i], cycle), get_pin_cycle(node->output_pins[i]));
			}
			printf("\n");*/
		}

		// Enqueue child nodes which are ready, not already queued, and not already complete.
		int num_children = 0;
		nnode_t **children = get_children_of(node, &num_children);

		for (i = 0; i < num_children; i++)
		{
			nnode_t* node = children[i];

			if (!node->in_queue && is_node_ready(node, cycle) && !is_node_complete(node, cycle))
			{
				node->in_queue = TRUE;
				queue->add(queue,node);
				//printf("***** ADDED: %s (%d)\n", node->name, node->type);
			} else {
				//printf("***** NOT ADDED: %s (%d) [%d, %d, %d]\n", node->name, node->type, !node->in_queue, is_node_ready(node, cycle), !is_node_complete(node, cycle));
			}
		}
		free(children);

		node->in_queue = FALSE;
	}
	queue->destroy(queue);

	return reset_candidate;
}

int find_reset(nnode_t *node, void* visited, int value){
	if(node == NULL) return 1; // Shouldn't happen, but check just in case
	if(node->node_data == visited) return 1; // Already visited, shouldn't happen anyway

	/* Mark this node as visited */
	node->node_data = visited;

	if (node->type == FF_NODE){
		return 1;
	}

	//printf("****** %s(%d)\n", node->name, node->type);

	/* Iterate through every fanout node */
	int retVal = 1;
	int i, j;
	for(i = 0; i < node->num_output_pins; i++){
		if(node->output_pins[i] && node->output_pins[i]->net){
			for(j = 0; j < node->output_pins[i]->net->num_fanout_pins; j++){
				if(node->output_pins[i]->net->fanout_pins[j]){
					nnode_t *child = node->output_pins[i]->net->fanout_pins[j]->node;
					if(child){
						/* If this child hasn't already been visited, visit it now */
						if(child->node_data != visited){
							/* Visit children only if current value affects them*/
							retVal &= find_reset(child, visited, value);
						}
					}
				}
			}
		}
	}
	return retVal;
}


void convert_reset_to_init(netlist_t *netlist){
	/*Find potential resets*/
	int i;
	for(i = 0; i < netlist->num_top_input_nodes; i++){
		if(netlist->top_input_nodes[i]->type != CLOCK_NODE && (1 || strstr(netlist->top_input_nodes[i]->name, "reset"))){
			printf("**** Simulating Input: %s\n", netlist->top_input_nodes[i]->name);

			int up_zero = simulate_for_reset(netlist, netlist->top_input_nodes[i], 0, (signed char)1);
			if(up_zero != 1){
				//Clear simulation data
				reinitialize_simulation(netlist);
				break;
			}
			int up_one = simulate_for_reset(netlist, netlist->top_input_nodes[i], 1, (signed char)1);
			if(up_one == 0){
				//Clear simulation data
				reinitialize_simulation(netlist);
				break;
			}

			//Clear simulation data
			reinitialize_simulation(netlist);

			int down_zero = simulate_for_reset(netlist, netlist->top_input_nodes[i], 0, (signed char)0);
			if(down_zero != 1){
				//Clear simulation data
				reinitialize_simulation(netlist);
				break;
			}

			int down_one = simulate_for_reset(netlist, netlist->top_input_nodes[i], 1, (signed char)0);

			//Clear simulation data
			reinitialize_simulation(netlist);

			//printf("Results: %d, %d, %d, %d\n", up_zero, up_one, down_zero, down_one);

			if(up_zero==1 && up_one==1 && down_zero==1 && down_one==-1){
				printf("**** Potential Positive Reset Found: %s!\n", netlist->top_input_nodes[i]->name);
			}

			if(up_zero==1 && up_one==-1 && down_zero==1 && down_one==1){
				printf("**** Potential Negative Reset Found: %s!\n", netlist->top_input_nodes[i]->name);
			}
		}
	}
}

/* Perform the backwards and forward sweeps and remove the unused nodes */
void remove_unused_logic(netlist_t *netlist){
	mark_output_dependencies(netlist);
	identify_unused_nodes(netlist);
	remove_unused_nodes(&useless_nodes);
	calculate_addsub_statistics(&addsub_nodes);
	if(global_args.reset_elision){
		convert_reset_to_init(netlist);
	}
}
