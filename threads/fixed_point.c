#include "threads/fixed_point.h"
#include <stdio.h>
#include <string.h>



int int_to_fp(int n){
}/* integer를fixed point로전환*/
int fp_to_int_round(int x); /* FP를int로전환(반올림) */
int fp_to_int(int x); /* FP를int로전환(버림) */
int add_fp(int x, int y); /* FP의덧셈*/
int add_mixed(int x, int n);  /* FP와int의덧셈*/
int sub_fp(int x, int y); /*FP의뺄셈(x-y) */
int sub_mixed(int x, int n);  /* FP와int의뺄셈(x-n) */
int mult_fp(int x, int y);  /* FP의곱셈*/
int mult_mixed(int x,int y);  /* FP와int의곱셈*/
int div_fp(int x, int y); /* FP의나눗셈(x/y) */
int div_mixed(int x, int n);  /* FP와int나눗셈(x/n) */
