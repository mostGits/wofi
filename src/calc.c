#include "calc.h"

#include <stdlib.h>
#include <stdbool.h>

struct calc_parser {
        const char* s;
};

static void calc_skip_spaces(struct calc_parser* p) {
        while(*p->s == ' ' || *p->s == '\t' || *p->s == '\n') {
                ++p->s;
        }
}

static double calc_parse_expr(struct calc_parser* p, bool* ok);

static double calc_parse_number(struct calc_parser* p, bool* ok) {
        calc_skip_spaces(p);

        char* end = NULL;
        double value = strtod(p->s, &end);

        if(end == p->s) {
                *ok = false;
                return 0.0;
        }

        p->s = end;
        return value;
}

static double calc_parse_factor(struct calc_parser* p, bool* ok) {
        calc_skip_spaces(p);

        if(*p->s == '(') {
                ++p->s;
                double value = calc_parse_expr(p, ok);
                calc_skip_spaces(p);

                if(*p->s != ')') {
                        *ok = false;
                        return 0.0;
                }

                ++p->s;
                return value;
        }

        if(*p->s == '-') {
                ++p->s;
                return -calc_parse_factor(p, ok);
        }

        return calc_parse_number(p, ok);
}

static double calc_parse_term(struct calc_parser* p, bool* ok) {
        double value = calc_parse_factor(p, ok);

        while(*ok) {
                calc_skip_spaces(p);

                if(*p->s == '*') {
                        ++p->s;
                        value *= calc_parse_factor(p, ok);
                } else if(*p->s == '/') {
                        ++p->s;
                        double rhs = calc_parse_factor(p, ok);
                        if(!*ok) {
                                return 0.0;
                        }
                        if(rhs == 0.0) {
                                *ok = false;
                                return 0.0;
                        }
                        value /= rhs;
                } else {
                        break;
                }
        }

        return value;
}

static double calc_parse_expr(struct calc_parser* p, bool* ok) {
        double value = calc_parse_term(p, ok);

        while(*ok) {
                calc_skip_spaces(p);

                if(*p->s == '+') {
                        ++p->s;
                        value += calc_parse_term(p, ok);
                } else if(*p->s == '-') {
                        ++p->s;
                        value -= calc_parse_term(p, ok);
                } else {
                        break;
                }
        }

        return value;
}

bool calc_input_is_expression(const char* text) {
        if(text == NULL || *text == '\0') {
                return false;
        }

        bool has_digit = false;

        for(const char* p = text; *p; ++p) {
                char c = *p;

                if(c >= '0' && c <= '9') {
                        has_digit = true;
                        continue;
                }

                if(c == ' ' || c == '.' || c == '+' || c == '-' ||
                   c == '*' || c == '/' || c == '(' || c == ')') {
                        continue;
                }

                return false;
        }

        return has_digit;
}

bool calc_eval_expression(const char* text, double* out) {
        if(text == NULL || out == NULL) {
                return false;
        }

        struct calc_parser p = { .s = text };
        bool ok = true;

        double value = calc_parse_expr(&p, &ok);
        calc_skip_spaces(&p);

        if(!ok || *p.s != '\0') {
                return false;
        }

        *out = value;
        return true;
}
