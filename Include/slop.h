/* 

   SlopPy: A Python interpreter that facilitates sloppy, error-tolerant
   data parsing and analysis
  
   Copyright 2010 Philip J. Guo (pg@cs.stanford.edu). All rights reserved.

   This code carries the same license as the enclosing Python
   distribution: http://www.python.org/psf/license/ 

*/

#ifndef Py_SLOP_H
#define Py_SLOP_H
#ifdef __cplusplus
extern "C" {
#endif

#include "Python.h"
#define PYPRINT(obj) do {PyObject_Print(obj, stdout, 0); printf("\n");} while(0)

#ifdef __cplusplus
}
#endif
#endif /* !Py_SLOP_H */
