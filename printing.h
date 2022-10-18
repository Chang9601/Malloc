#ifndef PRINTING_H
#define PRINTING_H

/* Define printFormatter to be a function pointer type taking a sinle parameter
 * (a Header pointer) and returning void
 *
 * This allows the print freelist and tags functions to take various printing
 * functions depending on the kind of output desired
 *
 * https://www.cprogramming.com/tutorial/function-pointers.html
 */
typedef void (*printFormatter)(Header *);

/* Functions defining a format to print */
void basic_print(Header * block);
void print_list(Header * block);
void print_object(Header * block);
void print_status(Header * block);

/* Functions to print the freelist and boundary tag data structures taking a
 * one of the above printing functions as a function pointer
 */
void freelist_print(printFormatter pf);
void tags_print(printFormatter pf);

/* Helpers */
void print_sublist(printFormatter pf, Header * start, Header * end);
void print_pointer(void * p);

#endif // PRINTING_H
