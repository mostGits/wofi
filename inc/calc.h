#ifndef CALC_H
#define CALC_H

#include <stdbool.h>

bool calc_input_is_expression(const char* text);
bool calc_eval_expression(const char* text, double* out);

#endif
