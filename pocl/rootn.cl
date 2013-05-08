/* OpenCL built-in library: rootn()

   Copyright (c) 2011 Universidad Rey Juan Carlos
   
   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:
   
   The above copyright notice and this permission notice shall be included in
   all copies or substantial portions of the Software.
   
   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
   THE SOFTWARE.
*/

#include "templates.h"

/* We don't have type conversions yet */
// DEFINE_EXPR_V_VJ(rootn, pow(a, (stype)1.0 / convert_##vtype(b)))
// DEFINE_EXPR_V_VI(rootn, pow(a, (vtype)((stype)1.0 / (stype)b)))

// Define pseudo builtins
#define __builtin_rootnf(a,b) pow(a, 1.0f / (float) b)
#define __builtin_rootn(a,b)  pow(a, 1.0  / (double)b)

DEFINE_BUILTIN_V_VJ(rootn)
DEFINE_BUILTIN_V_VI(rootn)
