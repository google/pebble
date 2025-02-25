/*
 * Copyright 2024 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "order.h"

typedef struct ListNode {
  struct ListNode* next;
  struct ListNode* prev;
} ListNode;

typedef bool (*ListFilterCallback)(ListNode *found_node, void *data);

//! - If a callback returns true, the iteration continues
//! - If a callback returns false, the iteration stops.
typedef bool (*ListForEachCallback)(ListNode *node, void *context);

#define LIST_NODE_NULL { .next = NULL, .prev = NULL }

//! Initializes the node.
void list_init(ListNode *head);

//! Inserts new_node after node in the list.
//! Always returns one of the two nodes that is closest to, or is the tail of the list.
ListNode* list_insert_after(ListNode *node, ListNode *new_node);

//! Inserts new_node before node in the list.
//! Always returns one of the two nodes that is closest to, or is the head of the list.
//! Warning: Returns new_node, rather than the new head of the list
//! as you might expect.
ListNode* list_insert_before(ListNode *node, ListNode *new_node);

//! Removes the head of the list and returns the new head.
ListNode* list_pop_head(ListNode *node);

//! Removes the tail of the list and returns the new tail.
ListNode* list_pop_tail(ListNode *node);

//! Removed the node from the list.
//! @param node the ListNode to remove.
//! @param[in,out] *head will be updated if the removed node happens to be the head
//! @param[in,out] *tail will be updated if the removed node happens to be the tail.
//! @note head and tail parameters are optional. Pass in NULL if not used.
void list_remove(ListNode *node, ListNode **head, ListNode **tail);

//! Appends new_node to the tail of the list that node is part of.
//! @param node Any node in the list, can be NULL (will result in a list containing only new_node)
//! Always returns the tail of the list.
ListNode* list_append(ListNode *node, ListNode *new_node);

//! Appends new_node to the head of the list that node is part of.
//! @param node Any node in the list, can be NULL (will result in a list containing only new_node)
//! Always returns the head of the list.
ListNode* list_prepend(ListNode *node, ListNode *new_node);

//! Gets the next node
ListNode* list_get_next(ListNode *node);

//! Gets the previous node
ListNode* list_get_prev(ListNode *node);

//! Gets the last node in the list
ListNode* list_get_tail(ListNode *node);

//! Gets the first node in the list
ListNode* list_get_head(ListNode *node);

//! @return true if the passed in node is the head of a list.
bool list_is_head(const ListNode *node);

//! @return true if the passed in node is the tail of a list.
bool list_is_tail(const ListNode *node);

//! Counts the number of nodes from node to the tail of the list, including said node
uint32_t list_count_to_tail_from(ListNode *node);

//! Counts the number of nodes from node to the head of the list, including said node
uint32_t list_count_to_head_from(ListNode *node);

//! Counts the number of nodes from head to tail
uint32_t list_count(ListNode *node);

//! Gets the node at <index> away, where positive index is towards the tail
ListNode* list_get_at(ListNode *node, int32_t index);

//! Adds a node to a list ordered by given comparator.
//! @param[in] head The head of the list that we want to add to.
//! @param[in] new_node The node being added.
//! @param[in] comparator The comparison function to use
//! @param[in] ascending True to maintain the list ordered ascending from head to tail.
//! @returns The (new) head of the list.
//! @note This function will not sort existing nodes in the list.
ListNode* list_sorted_add(ListNode *head, ListNode *new_node, Comparator comparator, bool ascending);

//! @param[in] head The head of the list to search.
//! @param[in] node The node to search for.
//! @returns True if the list contains node
bool list_contains(const ListNode *head, const ListNode *node);

//! Gets the first node that conforms to the given filter callback
//! @param node The list node from which to depart the search
//! @param filter_callback A function returning true in case the node that is passed in matches the
//! filter criteria, and false if it doesn't and should be skipped.
//! @param found_node The node to be evaluated by the filter callback
//! @param data Optional callback data
ListNode* list_find(ListNode *node, ListFilterCallback filter_callback, void *data);

//! Gets the next node that conforms to the given filter callback
//! @param node The list node from which to depart the search
//! @param filter_callback A function returning true in case the node that is passed in matches the
//! filter criteria, and false if it doesn't and should be skipped.
//! @param found_node The node to be evaluated by the filter callback
//! @param wrap_around True if the search should continue from the head if the tail has been reached
//! @param data Optional callback data
ListNode* list_find_next(ListNode *node, ListFilterCallback filter_callback, bool wrap_around, void *data);

//! Gets the previous node that conforms to the given filter callback
//! @param node The list node from which to depart the search
//! @param filter_callback A function returning true in case the node that is passed in matches the
//! filter criteria, and false if it doesn't and should be skipped.
//! @param found_node The node to be evaluated by the filter callback
//! @param wrap_around True if the search should continue from the tail if the head has been reached
//! @param data Optional callback data
ListNode* list_find_prev(ListNode *node, ListFilterCallback filter_callback, bool wrap_around, void *data);

//! Concatenate two lists.
//! @param list_a list onto which to concatenate list_b
//! @param list_b list to concatenate onto list_a
//! @return head of the new list
ListNode* list_concatenate(ListNode *list_a, ListNode *list_b);

//! Iterates over each node and passes it into callback given
//! @param[in] head The head of the list that we want to iterate over.
//! @param[in] each_cb The callback function to pass each node into
//! @param[in] data Optional callback data
void list_foreach(ListNode *head, ListForEachCallback each_cb, void *context);

//! Dump a list to PBL_LOG
void list_debug_dump(ListNode *head);

