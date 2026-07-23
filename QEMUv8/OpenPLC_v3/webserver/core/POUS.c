#include "openplc_cfi.h"
#define CFI_PROTECT 1
void PROGRAM0_init__(PROGRAM0 *data__, BOOL retain) {
  #if CFI_PROTECT
  /* CFI: matiec_generated, tag=0x6B8C3BD4U */
  CFI_PROLOGUE_TAG(0x6B8C3BD4U);
  #endif

  __INIT_VAR(data__->VALVE_IN,__BOOL_LITERAL(FALSE),retain)
  __INIT_VAR(data__->VALVE_IN_INPUT,__BOOL_LITERAL(FALSE),retain)
  __INIT_VAR(data__->VALVE_OUT,__BOOL_LITERAL(FALSE),retain)
  __INIT_VAR(data__->VALVE_OUT_INPUT,__BOOL_LITERAL(FALSE),retain)
  __INIT_EXTERNAL(REAL,WATER_HEIGHT,data__->WATER_HEIGHT,retain)
  __INIT_VAR(data__->WATER_HEIGH_MIN,0.0,retain)
  __INIT_VAR(data__->WATER_HEIGHT_MAX,100.0,retain)
  __INIT_VAR(data__->INNER_WATER_HEIGHT,100.0,retain)
  __INIT_EXTERNAL(REAL,VALVE_IN_RATE,data__->VALVE_IN_RATE,retain)
  __INIT_VAR(data__->VALVE_IN_RATE_MIN,0.0,retain)
  __INIT_VAR(data__->VALVE_IN_RATE_MAX,3.0,retain)
  __INIT_VAR(data__->INNER_VALVE_IN_RATE,0.0,retain)
  __INIT_VAR(data__->VALVE_OUT_RATE,0.3,retain)
  __INIT_VAR(data__->_TMP_BOOL_TO_REAL41_OUT,0,retain)
  __INIT_VAR(data__->_TMP_MUL17_OUT,0,retain)
  __INIT_VAR(data__->_TMP_ADD40_OUT,0,retain)
  __INIT_VAR(data__->_TMP_BOOL_TO_REAL12_OUT,0,retain)
  __INIT_VAR(data__->_TMP_MUL23_OUT,0,retain)
  __INIT_VAR(data__->_TMP_SQRT51_OUT,0,retain)
  __INIT_VAR(data__->_TMP_MUL27_OUT,0,retain)
  __INIT_VAR(data__->_TMP_SUB22_OUT,0,retain)
  __INIT_VAR(data__->_TMP_LIMIT52_OUT,0,retain)
  __INIT_VAR(data__->_TMP_LIMIT14_OUT,0,retain)
  #if CFI_PROTECT
  CFI_EPILOGUE_TAG_PAC(0x6B8C3BD4U);
  #endif
}

// Code part
void PROGRAM0_body__(PROGRAM0 *data__) {
  // Initialise TEMP variables

  #if CFI_PROTECT
  /* CFI: matiec_generated, tag=0x855CE55FU */
  CFI_PROLOGUE_TAG(0x855CE55FU);
  #endif

  __SET_VAR(data__->,VALVE_IN,,!(__GET_VAR(data__->VALVE_IN_INPUT,)));
  __SET_VAR(data__->,VALVE_OUT,,!(__GET_VAR(data__->VALVE_OUT_INPUT,)));
  __SET_VAR(data__->,_TMP_BOOL_TO_REAL41_OUT,,BOOL_TO_REAL(
    (BOOL)__BOOL_LITERAL(TRUE),
    NULL,
    (BOOL)__GET_VAR(data__->VALVE_IN,)));
  __SET_VAR(data__->,_TMP_MUL17_OUT,,MUL__REAL__REAL(
    (BOOL)__BOOL_LITERAL(TRUE),
    NULL,
    (UINT)2,
    (REAL)__GET_VAR(data__->_TMP_BOOL_TO_REAL41_OUT,),
    (REAL)__GET_VAR(data__->INNER_VALVE_IN_RATE,)));
  __SET_VAR(data__->,_TMP_ADD40_OUT,,ADD__REAL__REAL(
    (BOOL)__BOOL_LITERAL(TRUE),
    NULL,
    (UINT)2,
    (REAL)__GET_VAR(data__->_TMP_MUL17_OUT,),
    (REAL)__GET_VAR(data__->INNER_WATER_HEIGHT,)));
  __SET_VAR(data__->,INNER_WATER_HEIGHT,,__GET_VAR(data__->_TMP_ADD40_OUT,));
  __SET_VAR(data__->,_TMP_BOOL_TO_REAL12_OUT,,BOOL_TO_REAL(
    (BOOL)__BOOL_LITERAL(TRUE),
    NULL,
    (BOOL)__GET_VAR(data__->VALVE_OUT,)));
  __SET_VAR(data__->,_TMP_MUL23_OUT,,MUL__REAL__REAL(
    (BOOL)__BOOL_LITERAL(TRUE),
    NULL,
    (UINT)2,
    (REAL)__GET_VAR(data__->_TMP_BOOL_TO_REAL12_OUT,),
    (REAL)__GET_VAR(data__->VALVE_OUT_RATE,)));
  __SET_VAR(data__->,_TMP_SQRT51_OUT,,SQRT__REAL__REAL(
    (BOOL)__BOOL_LITERAL(TRUE),
    NULL,
    (REAL)__GET_VAR(data__->INNER_WATER_HEIGHT,)));
  __SET_VAR(data__->,_TMP_MUL27_OUT,,MUL__REAL__REAL(
    (BOOL)__BOOL_LITERAL(TRUE),
    NULL,
    (UINT)2,
    (REAL)__GET_VAR(data__->_TMP_MUL23_OUT,),
    (REAL)__GET_VAR(data__->_TMP_SQRT51_OUT,)));
  __SET_VAR(data__->,_TMP_SUB22_OUT,,SUB__REAL__REAL__REAL(
    (BOOL)__BOOL_LITERAL(TRUE),
    NULL,
    (REAL)__GET_VAR(data__->INNER_WATER_HEIGHT,),
    (REAL)__GET_VAR(data__->_TMP_MUL27_OUT,)));
  __SET_VAR(data__->,INNER_WATER_HEIGHT,,__GET_VAR(data__->_TMP_SUB22_OUT,));
  __SET_VAR(data__->,_TMP_LIMIT52_OUT,,LIMIT__REAL__REAL__REAL__REAL(
    (BOOL)__BOOL_LITERAL(TRUE),
    NULL,
    (REAL)__GET_VAR(data__->WATER_HEIGH_MIN,),
    (REAL)__GET_VAR(data__->INNER_WATER_HEIGHT,),
    (REAL)__GET_VAR(data__->WATER_HEIGHT_MAX,)));
  __SET_EXTERNAL(data__->,WATER_HEIGHT,,__GET_VAR(data__->_TMP_LIMIT52_OUT,));
  __SET_VAR(data__->,_TMP_LIMIT14_OUT,,LIMIT__REAL__REAL__REAL__REAL(
    (BOOL)__BOOL_LITERAL(TRUE),
    NULL,
    (REAL)__GET_VAR(data__->VALVE_IN_RATE_MIN,),
    (REAL)__GET_EXTERNAL(data__->VALVE_IN_RATE,),
    (REAL)__GET_VAR(data__->VALVE_IN_RATE_MAX,)));
  __SET_VAR(data__->,INNER_VALVE_IN_RATE,,__GET_VAR(data__->_TMP_LIMIT14_OUT,));

  goto __end;

__end:
  #if CFI_PROTECT
  CFI_EPILOGUE_TAG_PAC(0x855CE55FU);
  #endif
  return;
} // PROGRAM0_body__() 





void PROGRAM1_init__(PROGRAM1 *data__, BOOL retain) {
  #if CFI_PROTECT
  /* CFI: matiec_generated, tag=0x12F07DB6U */
  CFI_PROLOGUE_TAG(0x12F07DB6U);
  #endif

  PID_init__(&data__->PID0,retain);
  __INIT_VAR(data__->PID_AUTO,__BOOL_LITERAL(FALSE),retain)
  __INIT_VAR(data__->PID_X0,0.3,retain)
  __INIT_VAR(data__->PID_KP,-0.3,retain)
  __INIT_VAR(data__->PID_TR,1.3,retain)
  __INIT_VAR(data__->PID_TD,1.0,retain)
  __INIT_VAR(data__->PID_CYCLE,__time_to_timespec(1, 200, 0, 0, 0, 0),retain)
  __INIT_EXTERNAL(REAL,WATER_HEIGHT,data__->WATER_HEIGHT,retain)
  __INIT_EXTERNAL(REAL,VALVE_IN_RATE,data__->VALVE_IN_RATE,retain)
  __INIT_VAR(data__->SETPOINT,50.0,retain)
  #if CFI_PROTECT
  CFI_EPILOGUE_TAG_PAC(0x12F07DB6U);
  #endif
}

// Code part
void PROGRAM1_body__(PROGRAM1 *data__) {
  // Initialise TEMP variables

  #if CFI_PROTECT
  /* CFI: matiec_generated, tag=0x358D82E7U */
  CFI_PROLOGUE_TAG(0x358D82E7U);
  #endif

  __SET_VAR(data__->PID0.,AUTO,,!(__GET_VAR(data__->PID_AUTO,)));
  __SET_VAR(data__->PID0.,PV,,__GET_EXTERNAL(data__->WATER_HEIGHT,));
  __SET_VAR(data__->PID0.,SP,,__GET_VAR(data__->SETPOINT,));
  __SET_VAR(data__->PID0.,X0,,__GET_VAR(data__->PID_X0,));
  __SET_VAR(data__->PID0.,KP,,__GET_VAR(data__->PID_KP,));
  __SET_VAR(data__->PID0.,TR,,__GET_VAR(data__->PID_TR,));
  __SET_VAR(data__->PID0.,TD,,__GET_VAR(data__->PID_TD,));
  __SET_VAR(data__->PID0.,CYCLE,,__GET_VAR(data__->PID_CYCLE,));
  PID_body__(&data__->PID0);
  __SET_EXTERNAL(data__->,VALVE_IN_RATE,,__GET_VAR(data__->PID0.XOUT,));

  goto __end;

__end:
  #if CFI_PROTECT
  CFI_EPILOGUE_TAG_PAC(0x358D82E7U);
  #endif
  return;
} // PROGRAM1_body__() 





