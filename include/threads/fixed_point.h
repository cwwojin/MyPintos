#define F (1<< 14)//fixed point 1
#define INT_MAX((1<< 31) -1)
#define INT_MIN(-(1<< 31))
// x and y denote fixed_pointnumbers in 17.14 format
// n is an integer


intint_to_fp(intn);/* integer를fixed point로전환*/
intfp_to_int_round(intx);/* FP를int로전환(반올림) */
intfp_to_int(intx);/* FP를int로전환(버림) */
intadd_fp(intx,inty);/* FP의덧셈*/
intadd_mixed(intx, intn);/* FP와int의덧셈*/
intsub_fp(intx, inty);/* FP의뺄셈(x-y) */
intsub_mixed(intx, intn);/* FP와int의뺄셈(x-n) */
intmult_fp(intx, inty);/* FP의곱셈*/
intmult_mixed(intx,inty);/* FP와int의곱셈*/
intdiv_fp(intx, inty);/* FP의나눗셈(x/y) */
intdiv_mixed(intx, intn);/* FP와int나눗셈(x/n) */
