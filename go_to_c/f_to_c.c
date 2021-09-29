#include "f_to_c.h"
#include <stdio.h>

double f_to_c(int fahr) {
   double c = ((fahr - 32) * 5.0)/9.0; 
   return c; 
}

