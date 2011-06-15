#include <libint/libint.h>
#include "libderiv.h"

void deriv_build_BZ_d(prim_data *Data, const int a_num, const int cd_num, double *vp, const double *I0, const double *I1)
{
  const double twotzeta = Data->twozeta_b;
  const double *i0, *i1;
  int a,cd;

  for(a=0;a<a_num;a++) {
  i0 = I0 + 2*cd_num;
  for(cd=0;cd<cd_num;cd++)
    *(vp++) = twotzeta*(*(i0++)) ;
  i0 = I0 + 4*cd_num;
  for(cd=0;cd<cd_num;cd++)
    *(vp++) = twotzeta*(*(i0++)) ;
  i0 = I0 + 5*cd_num;
  i1 = I1;
  for(cd=0;cd<cd_num;cd++)
    *(vp++) = twotzeta*(*(i0++)) - 1.000000*(*(i1++));
  i0 = I0 + 7*cd_num;
  for(cd=0;cd<cd_num;cd++)
    *(vp++) = twotzeta*(*(i0++)) ;
  i0 = I0 + 8*cd_num;
  i1 = I1 + 1*cd_num;
  for(cd=0;cd<cd_num;cd++)
    *(vp++) = twotzeta*(*(i0++)) - 1.000000*(*(i1++));
  i0 = I0 + 9*cd_num;
  i1 = I1 + 2*cd_num;
  for(cd=0;cd<cd_num;cd++)
    *(vp++) = twotzeta*(*(i0++)) - 2.000000*(*(i1++));
  I0 += 10*cd_num;  I1 += 3*cd_num;
  }
}