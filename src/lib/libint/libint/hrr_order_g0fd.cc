#include <stdio.h>
#include <string.h>
#include "libint.h"
#include "hrr_header.h"

extern void vrr_order_g0fd(Libint_t*, prim_data*);

  /* Computes quartets of (g0|fd) integrals */

REALTYPE *hrr_order_g0fd(Libint_t *Libint, int num_prim_comb)
{
 prim_data *Data = Libint->PrimQuartet;
 REALTYPE *int_stack = Libint->int_stack;
 int i;

 Libint->vrr_classes[4][3] = int_stack + 0;
 Libint->vrr_classes[4][4] = int_stack + 150;
 Libint->vrr_classes[4][5] = int_stack + 375;
 memset(int_stack,0,690*sizeof(REALTYPE));

 Libint->vrr_stack = int_stack + 690;
 for(i=0;i<num_prim_comb;i++) {
   vrr_order_g0fd(Libint, Data);
   Data++;
 }
 /*--- compute (g0|fp) ---*/
 hrr3_build_fp(Libint->CD,int_stack+690,int_stack+150,int_stack+0,15);
 /*--- compute (g0|gp) ---*/
 hrr3_build_gp(Libint->CD,int_stack+1140,int_stack+375,int_stack+150,15);
 /*--- compute (g0|fd) ---*/
 hrr3_build_fd(Libint->CD,int_stack+1815,int_stack+1140,int_stack+690,15);
 return int_stack+1815;}