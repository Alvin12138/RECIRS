PROGRAM program0
  VAR
    Valve_in : BOOL;
  END_VAR
  VAR_INPUT
    Valve_in_input : BOOL;
  END_VAR
  VAR
    Valve_out : BOOL;
  END_VAR
  VAR_INPUT
    Valve_out_input : BOOL;
  END_VAR
  VAR_EXTERNAL
    water_height : REAL;
  END_VAR
  VAR
    water_heigh_MIN : REAL := 0.0;
    water_height_MAX : REAL := 100.0;
    inner_water_height : REAL := 100.0;
  END_VAR
  VAR_EXTERNAL
    Valve_in_rate : REAL;
  END_VAR
  VAR
    Valve_in_rate_MIN : REAL := 0.0;
    Valve_in_rate_MAX : REAL := 3.0;
    inner_Valve_in_rate : REAL := 0.0;
    Valve_out_rate : REAL := 0.3;
    _TMP_BOOL_TO_REAL41_OUT : REAL;
    _TMP_MUL17_OUT : REAL;
    _TMP_ADD40_OUT : REAL;
    _TMP_BOOL_TO_REAL12_OUT : REAL;
    _TMP_MUL23_OUT : REAL;
    _TMP_SQRT51_OUT : REAL;
    _TMP_MUL27_OUT : REAL;
    _TMP_SUB22_OUT : REAL;
    _TMP_LIMIT52_OUT : REAL;
    _TMP_LIMIT14_OUT : REAL;
  END_VAR

  Valve_in := NOT(Valve_in_input);
  Valve_out := NOT(Valve_out_input);
  _TMP_BOOL_TO_REAL41_OUT := BOOL_TO_REAL(Valve_in);
  _TMP_MUL17_OUT := MUL(_TMP_BOOL_TO_REAL41_OUT, inner_Valve_in_rate);
  _TMP_ADD40_OUT := ADD(_TMP_MUL17_OUT, inner_water_height);
  inner_water_height := _TMP_ADD40_OUT;
  _TMP_BOOL_TO_REAL12_OUT := BOOL_TO_REAL(Valve_out);
  _TMP_MUL23_OUT := MUL(_TMP_BOOL_TO_REAL12_OUT, Valve_out_rate);
  _TMP_SQRT51_OUT := SQRT(inner_water_height);
  _TMP_MUL27_OUT := MUL(_TMP_MUL23_OUT, _TMP_SQRT51_OUT);
  _TMP_SUB22_OUT := SUB(inner_water_height, _TMP_MUL27_OUT);
  inner_water_height := _TMP_SUB22_OUT;
  _TMP_LIMIT52_OUT := LIMIT(water_heigh_MIN, inner_water_height, water_height_MAX);
  water_height := _TMP_LIMIT52_OUT;
  _TMP_LIMIT14_OUT := LIMIT(Valve_in_rate_MIN, Valve_in_rate, Valve_in_rate_MAX);
  inner_Valve_in_rate := _TMP_LIMIT14_OUT;
END_PROGRAM

PROGRAM program1
  VAR
    PID0 : PID;
    PID_Auto : BOOL;
    PID_X0 : REAL := 0.3;
    PID_KP : REAL := -0.3;
    PID_TR : REAL := 1.3;
    PID_TD : REAL := 1.0;
    PID_CYCLE : TIME := T#200ms;
  END_VAR
  VAR_EXTERNAL
    water_height : REAL;
    Valve_in_rate : REAL;
  END_VAR
  VAR_INPUT
    setpoint : REAL := 50.0;
  END_VAR

  PID0(AUTO := NOT(PID_Auto), PV := water_height, SP := setpoint, X0 := PID_X0, KP := PID_KP, TR := PID_TR, TD := PID_TD, CYCLE := PID_CYCLE);
  Valve_in_rate := PID0.XOUT;
END_PROGRAM


CONFIGURATION Config0

  RESOURCE Res0 ON PLC
    VAR_GLOBAL
      water_height : REAL := 100.0;
      Valve_in_rate : REAL := 0.0;
    END_VAR
    TASK task0(INTERVAL := T#200ms,PRIORITY := 0);
    TASK task1(INTERVAL := T#200ms,PRIORITY := 0);
    PROGRAM instance0 WITH task0 : program0;
    PROGRAM instance1 WITH task1 : program1;
  END_RESOURCE
END_CONFIGURATION
