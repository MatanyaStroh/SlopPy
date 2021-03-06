/*

   SlopPy: A Python interpreter that facilitates sloppy, error-tolerant
   data parsing and analysis

   Copyright 2010 Philip J. Guo (pg@cs.stanford.edu). All rights reserved.

   This code carries the same license as the enclosing Python
   distribution: http://www.python.org/psf/license/

*/

#include "slop.h"
#include "frameobject.h"
#include "opcode.h"
#include "code.h"


// start this at 0 and then only set to 1 after pg_initialize()
int pg_activated = 0;

FILE* verbose_log_file = NULL;
FILE* binary_log_file = NULL;

// References to Python standard library functions:
PyObject* cPickle_dumpstr_func = NULL;     // cPickle.dumps
PyObject* b64encode_func = NULL;           // base64.b64encode


// are we currently within a 'try' block (even transitively for ANY
// function on stack?)
int transitively_within_try_block() {
  PyFrameObject* f = PyEval_GetFrame();
  while (f) {
    int i;
    for (i = 0; i < f->f_iblock; i++) {
      PyTryBlock* b = &f->f_blockstack[i];
      if (b->b_type == SETUP_FINALLY || b->b_type == SETUP_EXCEPT) {
        return 1;
      }
    }

    f = f->f_back;
  }

  return 0;
}


/* information about static effects of bytecodes on stack taken from byteplay:
   http://code.google.com/p/byteplay/source/browse/trunk/byteplay.py

class _se:
    """Quick way of defining static stack effects of opcodes"""
    # Taken from assembler.py by Phillip J. Eby
    NOP = 0
    
    POP_TOP   = 1,0
    ROT_TWO   = 2,2
    ROT_THREE = 3,3
    ROT_FOUR  = 4,4
    DUP_TOP   = 1,2

    UNARY_POSITIVE = UNARY_NEGATIVE = UNARY_NOT = UNARY_CONVERT = \
        UNARY_INVERT = GET_ITER = LOAD_ATTR = IMPORT_NAME = 1,1

    IMPORT_FROM = 1,2

    BINARY_POWER = BINARY_MULTIPLY = BINARY_DIVIDE = BINARY_FLOOR_DIVIDE = \
        BINARY_TRUE_DIVIDE = BINARY_MODULO = BINARY_ADD = BINARY_SUBTRACT = \
        BINARY_SUBSCR = BINARY_LSHIFT = BINARY_RSHIFT = BINARY_AND = \
        BINARY_XOR = BINARY_OR = COMPARE_OP = 2,1

    INPLACE_POWER = INPLACE_MULTIPLY = INPLACE_DIVIDE = \
        INPLACE_FLOOR_DIVIDE = INPLACE_TRUE_DIVIDE = INPLACE_MODULO = \
        INPLACE_ADD = INPLACE_SUBTRACT = INPLACE_LSHIFT = INPLACE_RSHIFT = \
        INPLACE_AND = INPLACE_XOR = INPLACE_OR = 2,1

    SLICE_0, SLICE_1, SLICE_2, SLICE_3 = \
        (1,1),(2,1),(2,1),(3,1)
    STORE_SLICE_0, STORE_SLICE_1, STORE_SLICE_2, STORE_SLICE_3 = \
        (2,0),(3,0),(3,0),(4,0)
    DELETE_SLICE_0, DELETE_SLICE_1, DELETE_SLICE_2, DELETE_SLICE_3 = \
        (1,0),(2,0),(2,0),(3,0)

    STORE_SUBSCR = 3,0
    DELETE_SUBSCR = STORE_ATTR = 2,0
    DELETE_ATTR = STORE_DEREF = 1,0
    PRINT_NEWLINE = 0,0
    PRINT_EXPR = PRINT_ITEM = PRINT_NEWLINE_TO = IMPORT_STAR = 1,0
    STORE_NAME = STORE_GLOBAL = STORE_FAST = 1,0
    PRINT_ITEM_TO = LIST_APPEND = 2,0

    LOAD_LOCALS = LOAD_CONST = LOAD_NAME = LOAD_GLOBAL = LOAD_FAST = \
        LOAD_CLOSURE = LOAD_DEREF = BUILD_MAP = 0,1

    DELETE_FAST = DELETE_GLOBAL = DELETE_NAME = 0,0

    EXEC_STMT = 3,0
    BUILD_CLASS = 3,1

    if python_version == '2.4':
        YIELD_VALUE = 1,0
        IMPORT_NAME = 1,1
    elif python_version == '2.6':
        STORE_MAP = 2,0
    else:
        YIELD_VALUE = 1,1
        IMPORT_NAME = 2,1

_se = dict((op, getattr(_se, opname[op]))
           for op in opcodes
           if hasattr(_se, opname[op]))


def getse(op, arg=None):
    """Get the stack effect of an opcode, as a (pop, push) tuple.

    If an arg is needed and is not given, a ValueError is raised.
    If op isn't a simple opcode, that is, the flow doesn't always continue
    to the next opcode, a ValueError is raised.
    """
    try:
        return _se[op]
    except KeyError:
        # Continue to opcodes with an effect that depends on arg
        pass

    if arg is None:
        raise ValueError, "Opcode stack behaviour depends on arg"

    def get_func_tup(arg, nextra):
        if arg > 0xFFFF:
            raise ValueError, "Can only split a two-byte argument"
        return (nextra + 1 + (arg & 0xFF) + 2*((arg >> 8) & 0xFF),
                1)

    if op == CALL_FUNCTION:
        return get_func_tup(arg, 0)
    elif op == CALL_FUNCTION_VAR:
        return get_func_tup(arg, 1)
    elif op == CALL_FUNCTION_KW:
        return get_func_tup(arg, 1)
    elif op == CALL_FUNCTION_VAR_KW:
        return get_func_tup(arg, 2)

    elif op == BUILD_TUPLE:
        return arg, 1
    elif op == BUILD_LIST:
        return arg, 1
    elif op == UNPACK_SEQUENCE:
        return 1, arg
    elif op == BUILD_SLICE:
        return arg, 1
    elif op == DUP_TOPX:
        return arg, arg*2
    elif op == RAISE_VARARGS:
        return 1+arg, 1
    elif op == MAKE_FUNCTION:
        return 1+arg, 1
    elif op == MAKE_CLOSURE:
        if python_version == '2.4':
            raise ValueError, "The stack effect of MAKE_CLOSURE depends on TOS"
        else:
            return 2+arg, 1
    else:
        raise ValueError, "The opcode %r isn't recognized or has a special "\
              "flow control" % op
*/


// ok, we're going to REALLY tediously create some global constant
// tables that contain the effects of individual bytecodes on the stack

// if these numbers are incorrect, then there will be gnarly bugs :)

// {# elts popped off of stack, # elts pushed onto stack}
//
// {-1, -1} value means "invalid opcode"
// {-2, -2} value means "opcode that isn't likely gonna come up in an exception"
// {-999, -999} value means "variable stack effect depending on args"
static int bytecode_stack_effects[][2] = {
  // bytecode numbers from Include/opcode.h

  {-1, -1}, //#define STOP_CODE	0
  {1, 0}, //#define POP_TOP		1
  {2, 2}, //#define ROT_TWO		2
  {3, 3}, //#define ROT_THREE	3
  {1, 2}, //#define DUP_TOP		4
  {4, 4}, //#define ROT_FOUR	5
  {-1, -1}, // 6
  {-1, -1}, // 7
  {-1, -1}, // 8
  {0, 0}, //#define NOP		9
  {1, 1}, //#define UNARY_POSITIVE	10
  {1, 1}, //#define UNARY_NEGATIVE	11
  {1, 1}, //#define UNARY_NOT	12
  {1, 1}, //#define UNARY_CONVERT	13
  {-1, -1}, // 14
  {1, 1}, //#define UNARY_INVERT	15
  {-1, -1}, // 16
  {-1, -1}, // 17
  {2, 0}, //#define LIST_APPEND	18
  {2, 1}, //#define BINARY_POWER	19
  {2, 1}, //#define BINARY_MULTIPLY	20
  {2, 1}, //#define BINARY_DIVIDE	21
  {2, 1}, //#define BINARY_MODULO	22
  {2, 1}, //#define BINARY_ADD	23
  {2, 1}, //#define BINARY_SUBTRACT	24
  {2, 1}, //#define BINARY_SUBSCR	25
  {2, 1}, //#define BINARY_FLOOR_DIVIDE 26
  {2, 1}, //#define BINARY_TRUE_DIVIDE 27
  {2, 1}, //#define INPLACE_FLOOR_DIVIDE 28
  {2, 1}, //#define INPLACE_TRUE_DIVIDE 29
  {1, 1}, //#define SLICE_0		30
/* Also uses 31-33 */
  {2, 1}, //#define SLICE_1		31
  {2, 1}, //#define SLICE_2		32
  {3, 1}, //#define SLICE_3		33
  {-1, -1}, // 34
  {-1, -1}, // 35
  {-1, -1}, // 36
  {-1, -1}, // 37
  {-1, -1}, // 38
  {-1, -1}, // 39
  {2, 0}, //#define STORE_SLICE_0	40
/* Also uses 41-43 */
  {3, 0}, //#define STORE_SLICE_1	41
  {3, 0}, //#define STORE_SLICE_2	42
  {4, 0}, //#define STORE_SLICE_3	43
  {-1, -1}, // 44
  {-1, -1}, // 45
  {-1, -1}, // 46
  {-1, -1}, // 47
  {-1, -1}, // 48
  {-1, -1}, // 49
  {1, 0}, //#define DELETE_SLICE_0 50
/* Also uses 51-53 */
  {2, 0}, //#define DELETE_SLICE_1 51
  {2, 0}, //#define DELETE_SLICE_2 52
  {3, 0}, //#define DELETE_SLICE_3 53
  {2, 0}, //#define STORE_MAP	54
  {2, 1}, //#define INPLACE_ADD	55
  {2, 1}, //#define INPLACE_SUBTRACT	56
  {2, 1}, //#define INPLACE_MULTIPLY	57
  {2, 1}, //#define INPLACE_DIVIDE	58
  {2, 1}, //#define INPLACE_MODULO	59
  {3, 0}, //#define STORE_SUBSCR	60
  {2, 0}, //#define DELETE_SUBSCR	61
  {2, 1}, //#define BINARY_LSHIFT	62
  {2, 1}, //#define BINARY_RSHIFT	63
  {2, 1}, //#define BINARY_AND	64
  {2, 1}, //#define BINARY_XOR	65
  {2, 1}, //#define BINARY_OR	66
  {2, 1}, //#define INPLACE_POWER	67
  {1, 1}, //#define GET_ITER	68
  {-1, -1}, // 69
  {1, 0}, //#define PRINT_EXPR	70
  {1, 0}, //#define PRINT_ITEM	71
  {0, 0}, //#define PRINT_NEWLINE	72
  {2, 0}, //#define PRINT_ITEM_TO   73
  {1, 0}, //#define PRINT_NEWLINE_TO 74
  {2, 1}, //#define INPLACE_LSHIFT	75
  {2, 1}, //#define INPLACE_RSHIFT	76
  {2, 1}, //#define INPLACE_AND	77
  {2, 1}, //#define INPLACE_XOR	78
  {2, 1}, //#define INPLACE_OR	79
  {-2, -2}, //#define BREAK_LOOP	80
  {-2, -2}, //#define WITH_CLEANUP    81
  {0, 1}, //#define LOAD_LOCALS	82
  {-2, -2}, //#define RETURN_VALUE	83
  {1, 0}, //#define IMPORT_STAR	84
  {3, 0}, //#define EXEC_STMT	85
  {1, 1}, //#define YIELD_VALUE	86 (TODO: uncertain about this one)
  {-2, -2}, //#define POP_BLOCK	87
  {-2, -2}, //#define END_FINALLY	88
  {3, 1}, //#define BUILD_CLASS	89

//#define HAVE_ARGUMENT	90	/* Opcodes from here have an argument: */

  {1, 0}, //#define STORE_NAME	90	/* Index in name list */
  {0, 0}, //#define DELETE_NAME	91	/* "" */
  {-999, -999}, //#define UNPACK_SEQUENCE	92	/* Number of sequence items */
  {-2, -2}, //#define FOR_ITER	93
  {-1, -1}, // 94
  {2, 0}, //#define STORE_ATTR	95	/* Index in name list */
  {1, 0}, //#define DELETE_ATTR	96	/* "" */
  {1, 0}, //#define STORE_GLOBAL	97	/* "" */
  {0, 0}, //#define DELETE_GLOBAL	98	/* "" */
  {-999, -999}, //#define DUP_TOPX	99	/* number of items to duplicate */
  {0, 1}, //#define LOAD_CONST	100	/* Index in const list */
  {0, 1}, //#define LOAD_NAME	101	/* Index in name list */
  {-999, -999}, //#define BUILD_TUPLE	102	/* Number of tuple items */
  {-999, -999}, //#define BUILD_LIST	103	/* Number of list items */
  {0, 1}, //#define BUILD_MAP	104	/* Always zero for now */
  {1, 1}, //#define LOAD_ATTR	105	/* Index in name list */
  {2, 1}, //#define COMPARE_OP	106	/* Comparison operator */
  {1, 1}, //#define IMPORT_NAME	107	/* Index in name list */
  {1, 2}, //#define IMPORT_FROM	108	/* Index in name list */
  {-1, -1}, // 109
  {-2, -2}, //#define JUMP_FORWARD	110	/* Number of bytes to skip */
  {-2, -2}, //#define JUMP_IF_FALSE	111	/* "" */
  {-2, -2}, //#define JUMP_IF_TRUE	112	/* "" */
  {-2, -2}, //#define JUMP_ABSOLUTE	113	/* Target byte offset from beginning of code */
  {-1, -1}, // 114
  {-1, -1}, // 115
  {0, 1}, //#define LOAD_GLOBAL	116	/* Index in name list */
  {-1, -1}, // 117
  {-1, -1}, // 118
  {-2, -2}, //#define CONTINUE_LOOP	119	/* Start of loop (absolute) */
  {-2, -2}, //#define SETUP_LOOP	120	/* Target address (relative) */
  {-2, -2}, //#define SETUP_EXCEPT	121	/* "" */
  {-2, -2}, //#define SETUP_FINALLY	122	/* "" */
  {-1, -1}, // 123
  {0, 1}, //#define LOAD_FAST	124	/* Local variable number */
  {1, 0}, //#define STORE_FAST	125	/* Local variable number */
  {0, 0}, //#define DELETE_FAST	126	/* Local variable number */
  {-1, -1}, // 127
  {-1, -1}, // 128
  {-1, -1}, // 129
  {-999, -999}, //#define RAISE_VARARGS	130	/* Number of raise arguments (1, 2 or 3) */

/* CALL_FUNCTION_XXX opcodes defined below depend on this definition */
  {-999, -999}, //#define CALL_FUNCTION	131	/* #args + (#kwargs<<8) */
  {-999, -999}, //#define MAKE_FUNCTION	132	/* #defaults */
  {-999, -999}, //#define BUILD_SLICE 	133	/* Number of items */
  {-999, -999}, //#define MAKE_CLOSURE    134     /* #free vars */
  {0, 1}, //#define LOAD_CLOSURE    135     /* Load free variable from closure */
  {0, 1}, //#define LOAD_DEREF      136     /* Load and dereference from closure cell */ 
  {1, 0}, //#define STORE_DEREF     137     /* Store into cell */ 
  {-1, -1}, // 138
  {-1, -1}, // 139

/* The next 3 opcodes must be contiguous and satisfy
   (CALL_FUNCTION_VAR - CALL_FUNCTION) & 3 == 1  */
  {-999, -999}, //#define CALL_FUNCTION_VAR          140	/* #args + (#kwargs<<8) */
  {-999, -999}, //#define CALL_FUNCTION_KW           141	/* #args + (#kwargs<<8) */
  {-999, -999}, //#define CALL_FUNCTION_VAR_KW       142	/* #args + (#kwargs<<8) */

/* Support for opargs more than 16 bits long */
  {-1, -1}, //#define EXTENDED_ARG  143
};


// see Include/slop.h for meanings of return values
int get_NA_stack_action(int opcode) {
  int num_elts_popped = bytecode_stack_effects[opcode][0];
  int num_elts_pushed = bytecode_stack_effects[opcode][1];

  if (num_elts_popped < 0 && num_elts_pushed < 0) {
    fprintf(stderr, "problematic opcode: %d\n", opcode);
    Py_FatalError("invalid opcode in get_NA_stack_action()");
  }

  // choose actions based on these 'rules' that I reverse-engineered:
  if (num_elts_pushed == 0) {
    return DO_NOTHING;
  }

  if (num_elts_popped >= num_elts_pushed) {
    if (num_elts_pushed == 1) {
      return DO_SET_TOP_1;
    }
  }
  else {
    if (num_elts_pushed == 1) {
      return DO_PUSH_1;
    }
  }

  fprintf(stderr, "problematic opcode: %d\n", opcode);
  Py_FatalError("unreachable code in get_NA_stack_action()");
  return NEED_SPECIAL_HANDLING;
}


// called at the beginning of execution
void pg_initialize(void) {
#ifdef DISABLE_MEMOIZE
  return;
#endif

  assert(!pg_activated);

  // import some useful Python modules, so that we can call their functions:
  PyObject* cPickle_module = PyImport_ImportModule("cPickle"); // increments refcount

  // this is the first C extension module we're trying to import, so do
  // a check to make sure it exists, and if not, simply punt on
  // initialization altogether.  This will ensure that SlopPy can compile
  // after a 'make clean' (since cPickle doesn't exist yet at that time)
  if (!cPickle_module) {
    fprintf(stderr, "WARNING: cPickle module doesn't yet exist, so SlopPy features not activated.\n");
    return;
  }

  assert(cPickle_module);
  cPickle_dumpstr_func = PyObject_GetAttrString(cPickle_module, "dumps");
  assert(cPickle_dumpstr_func);
  Py_DECREF(cPickle_module);

  PyObject* base64_module = PyImport_ImportModule("base64"); // increments refcount
  b64encode_func = PyObject_GetAttrString(base64_module, "b64encode");
  assert(b64encode_func);
  Py_DECREF(base64_module);

  if (!base64_module) {
    fprintf(stderr, "WARNING: base64 module doesn't yet exist, so SlopPy features not activated.\n");
    return;
  }

  verbose_log_file = fopen("slop_verbose.log", "w");
  binary_log_file = fopen("slop_binary.log", "w");

  pg_activated = 1;
}

// called at the end of execution
void pg_finalize(void) {
#ifdef DISABLE_MEMOIZE
  return;
#endif

  if (!pg_activated) {
    return;
  }

  // turn this off ASAP
  pg_activated = 0;

  Py_CLEAR(cPickle_dumpstr_func);
  Py_CLEAR(b64encode_func);

  fclose(verbose_log_file);
  verbose_log_file = NULL;

  fclose(binary_log_file);
  binary_log_file = NULL;
}


void log_NA_event(const char* event_name) {
  PyFrameObject* f = PyEval_GetFrame();

  // stolen from frame_getlineno in Objects/frameobject.c
  int lineno;
  if (f->f_trace)
    lineno = f->f_lineno;
  else
    lineno = PyCode_Addr2Line(f->f_code, f->f_lasti);


  PG_LOG_PRINTF("{%s | %s %s:%d}\n",
                event_name,
                PyString_AsString(f->f_code->co_filename),
                PyString_AsString(f->f_code->co_name),
                lineno);
}

