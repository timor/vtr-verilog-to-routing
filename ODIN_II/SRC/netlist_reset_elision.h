/*
 * netlist_reset_elision.h
 *
 *  Created on: Apr 19, 2016
 *      Author: CASA
 */

#ifndef ODIN_II_SRC_NETLIST_RESET_ELISION_H_
#define ODIN_II_SRC_NETLIST_RESET_ELISION_H_

void detect_and_remove_reset(netlist_t *netlist, FILE* file);
void exclude_inputs_directly_driving_outputs(netlist_t *netlist);
void traverse_from_outputs(nnode_t *node);
void check_reset_per_latch(netlist_t *netlist);
void traverse_check_reset_per_latch(nnode_t *node);
void check_latch_driver(nnode_t *node, nnode_t *latch_node);
void mark_input_as_reset(nnode_t *input_node, int is_positive_reset);
void remove_reset(netlist_t *netlist, nnode_t *reset_node);
void print_remove_reset(netlist_t *netlist, nnode_t *reset_node, FILE* file);
void string_replace(char *line, char *old_word, char *new_word);
void update_latch_initial(char* line, signed char init);

#endif /* ODIN_II_SRC_NETLIST_RESET_ELISION_H_ */
