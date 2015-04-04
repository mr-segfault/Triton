#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "pin.H"

#include "AnalysisProcessor.h"
#include "IRBuilder.h"
#include "IRBuilderFactory.h"
#include "Inst.h"
#include "PINContextHandler.h"
#include "PythonBindings.h"
#include "Trace.h"
#include "Trigger.h"


/* Pin options: -script */
KNOB<std::string>   KnobPythonModule(KNOB_MODE_WRITEONCE, "pintool", "script", "", "Python script");

AnalysisProcessor   ap;
Trace               trace;
Trigger             analysisTrigger = Trigger();




void applyPyConfBefore(IRBuilder *irb, CONTEXT *ctx, THREADID threadId)
{
  // Check if the DSE must be start at this address
  if (PyTritonOptions::startAnalysisFromAddr.find(irb->getAddress()) != PyTritonOptions::startAnalysisFromAddr.end())
    analysisTrigger.update(true);

  // Check if the DSE must be stop at this address
  if (PyTritonOptions::stopAnalysisFromAddr.find(irb->getAddress()) != PyTritonOptions::stopAnalysisFromAddr.end())
    analysisTrigger.update(false);

  // Check if there is registers tainted via the python bindings
  std::list<uint64_t> regsTainted = PyTritonOptions::taintRegFromAddr[irb->getAddress()];
  std::list<uint64_t>::iterator it1 = regsTainted.begin();
  for ( ; it1 != regsTainted.end(); it1++)
    ap.taintReg(*it1);

  // Check if there is registers untainted via the python bindings
  std::list<uint64_t> regsUntainted = PyTritonOptions::untaintRegFromAddr[irb->getAddress()];
  std::list<uint64_t>::iterator it2 = regsUntainted.begin();
  for ( ; it2 != regsUntainted.end(); it2++)
    ap.untaintReg(*it2);

  // Check if there is a callback wich must be called at each instruction instrumented
  if (analysisTrigger.getState() && PyTritonOptions::callbackBefore){
    PyObject *args = PyTuple_New(2);
    PyTuple_SetItem(args, 0, PyInt_FromLong(irb->getAddress())); // TODO: Find a way to convert irb to python module
    PyTuple_SetItem(args, 1, PyInt_FromLong(threadId));
    if (PyObject_CallObject(PyTritonOptions::callbackBefore, args) == NULL){
      PyErr_Print();
      exit(1);
    }
    Py_DECREF(args); /* Free the allocated tuple */
  }
}


void applyPyConfAfter(IRBuilder *irb, CONTEXT *ctx, THREADID threadId)
{
  // Check if there is a callback wich must be called at each instruction instrumented
  if (analysisTrigger.getState() && PyTritonOptions::callbackAfter){
    PyObject *args = PyTuple_New(2);
    PyTuple_SetItem(args, 0, PyInt_FromLong(irb->getAddress())); // TODO: Find a way to convert irb to python module
    PyTuple_SetItem(args, 1, PyInt_FromLong(threadId));
    if (PyObject_CallObject(PyTritonOptions::callbackAfter, args) == NULL){
      PyErr_Print();
      exit(1);
    }
    Py_DECREF(args); /* Free the allocated tuple */
  }
}


VOID callback(IRBuilder *irb, CONTEXT *ctx, BOOL hasEA, ADDRINT ea, THREADID threadId)
{
  /* Some configurations must be applied before processing */
  applyPyConfBefore(irb, ctx, threadId);

  if (!analysisTrigger.getState())
  // Analysis locked
    return;

  PINContextHandler ctxH(ctx, threadId);

  if (hasEA)
    irb->setup(ea);

  trace.addInstruction(irb->process(ctxH, ap));

  /* Some configurations must be applied after processing */
  applyPyConfAfter(irb, ctx, threadId);
}


VOID TRACE_Instrumentation(TRACE trace, VOID *v)
{
  for (BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl = BBL_Next(bbl)){
    for (INS ins = BBL_InsHead(bbl); INS_Valid(ins); ins = INS_Next(ins)) {
      IRBuilder *irb = createIRBuilder(ins);

      if (INS_MemoryOperandCount(ins) > 0)
        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR) callback,
            IARG_PTR, irb,
            IARG_CONTEXT,
            IARG_BOOL, true,
            IARG_MEMORYOP_EA, 0,
            IARG_THREAD_ID,
            IARG_END);
      else
        INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR) callback,
            IARG_PTR, irb,
            IARG_CONTEXT,
            IARG_BOOL, false,
            IARG_ADDRINT, 0,
            IARG_THREAD_ID,
            IARG_END);
    }
  }
}


void toggleWrapper(bool flag)
{
  analysisTrigger.update(flag);
}


VOID IMG_Instrumentation(IMG img, VOID *)
{
  /* This callback is used to lock and target the analysis */
  /* Mainly used to target an area */
  if (PyTritonOptions::startAnalysisFromSymbol == NULL)
    return;
  RTN targetRTN = RTN_FindByName(img, PyTritonOptions::startAnalysisFromSymbol);
  if (RTN_Valid(targetRTN)){
    RTN_Open(targetRTN);

    RTN_InsertCall(targetRTN,
        IPOINT_BEFORE,
        (AFUNPTR) toggleWrapper,
        IARG_BOOL, true,
        IARG_END);

    RTN_InsertCall(targetRTN,
        IPOINT_AFTER,
        (AFUNPTR) toggleWrapper,
        IARG_BOOL, false,
        IARG_END);

    RTN_Close(targetRTN);
  }
}


VOID Fini(INT32, VOID *)
{
  if (PyTritonOptions::dumpTrace == true)
    trace.display();

  if (PyTritonOptions::dumpStats == true)
    ap.displayStats();

  Py_Finalize();
}


// Usage function if Pin fail to start.
// Display the help message.
INT32 Usage()
{
  std::cerr << KNOB_BASE::StringKnobSummary() << std::endl;
  return -1;
}


int main(int argc, char *argv[])
{
  PIN_InitSymbols();
  PIN_SetSyntaxIntel();
  if(PIN_Init(argc, argv))
      return Usage();

  // Init Python Bindings
  initBindings();

  // Image callback
  IMG_AddInstrumentFunction(IMG_Instrumentation, NULL);

  // Instruction callback
  TRACE_AddInstrumentFunction(TRACE_Instrumentation, NULL);

  // End instrumentation callback
  PIN_AddFiniFunction(Fini, NULL);

  // Exec the python bindings file
  execBindings(KnobPythonModule.Value().c_str());

  return 0;
}

