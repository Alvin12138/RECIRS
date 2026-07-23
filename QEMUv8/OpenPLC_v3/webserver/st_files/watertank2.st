PROGRAM program0
  VAR
    Valve_1 AT %IX0.1 : BOOL;
    Valve_in_input AT %IX0.0 : BOOL;
    Valve_3 AT %IX0.2 : BOOL;
    Valve_out_input AT %IX0.3 : BOOL;
  END_VAR
  VAR_EXTERNAL
    water_height_1 : REAL;
    water_height_2 : REAL;
  END_VAR
  VAR
    water_heigh_MIN : REAL := 0.0;
    water_height_MAX : REAL := 100.0;
    inner_water_height_1 : REAL := 100.0;
    inner_water_height_2 : REAL := 100.0;
  END_VAR
  VAR_EXTERNAL
    Valve_1_rate : REAL;
  END_VAR
  VAR
    Valve_rate_MIN : REAL := 0.0;
    Valve_rate_MAX : REAL := 3.0;
    inner_Valve_1_rate : REAL := 0.0;
    inner_Valve_2_rate : REAL := 0.1;
    inner_Valve_3_rate : REAL := 0.0;
  END_VAR
  VAR_EXTERNAL
    Valve_3_rate : REAL;
  END_VAR
  VAR
    _TMP_LIMIT52_OUT : REAL;
    _TMP_LIMIT14_OUT : REAL;
    _TMP_BOOL_TO_REAL41_OUT : REAL;
    _TMP_MUL17_OUT : REAL;
    _TMP_ADD40_OUT : REAL;
    _TMP_BOOL_TO_REAL12_OUT : REAL;
    _TMP_MUL23_OUT : REAL;
    _TMP_SQRT51_OUT : REAL;
    _TMP_MUL27_OUT : REAL;
    _TMP_SUB22_OUT : REAL;
    _TMP_LIMIT34_OUT : REAL;
    _TMP_ADD21_OUT : REAL;
    _TMP_LIMIT43_OUT : REAL;
    _TMP_BOOL_TO_REAL63_OUT : REAL;
    _TMP_MUL50_OUT : REAL;
    _TMP_SUB62_OUT : REAL;
  END_VAR

  IF NOT(Valve_in_input) THEN
    Valve_1 := TRUE; (*set*)
  END_IF;
  IF Valve_in_input THEN
    Valve_1 := FALSE; (*reset*)
  END_IF;
  IF NOT(Valve_out_input) THEN
    Valve_3 := TRUE; (*set*)
  END_IF;
  _TMP_LIMIT52_OUT := LIMIT(water_heigh_MIN, inner_water_height_1, water_height_MAX);
  water_height_1 := _TMP_LIMIT52_OUT;
  IF Valve_out_input THEN
    Valve_3 := FALSE; (*reset*)
  END_IF;
  _TMP_LIMIT14_OUT := LIMIT(Valve_rate_MIN, Valve_1_rate, Valve_rate_MAX);
  inner_Valve_1_rate := _TMP_LIMIT14_OUT;
  _TMP_BOOL_TO_REAL41_OUT := BOOL_TO_REAL(Valve_1);
  _TMP_MUL17_OUT := MUL(_TMP_BOOL_TO_REAL41_OUT, inner_Valve_1_rate);
  _TMP_ADD40_OUT := ADD(_TMP_MUL17_OUT, inner_water_height_1);
  inner_water_height_1 := _TMP_ADD40_OUT;
  _TMP_BOOL_TO_REAL12_OUT := BOOL_TO_REAL(TRUE);
  _TMP_MUL23_OUT := MUL(_TMP_BOOL_TO_REAL12_OUT, inner_Valve_2_rate);
  _TMP_SQRT51_OUT := SQRT(inner_water_height_1);
  _TMP_MUL27_OUT := MUL(_TMP_MUL23_OUT, _TMP_SQRT51_OUT);
  _TMP_SUB22_OUT := SUB(inner_water_height_1, _TMP_MUL27_OUT);
  inner_water_height_1 := _TMP_SUB22_OUT;
  _TMP_LIMIT34_OUT := LIMIT(Valve_rate_MIN, Valve_3_rate, Valve_rate_MAX);
  inner_Valve_3_rate := _TMP_LIMIT34_OUT;
  _TMP_ADD21_OUT := ADD(_TMP_MUL27_OUT, inner_water_height_2);
  inner_water_height_2 := _TMP_ADD21_OUT;
  _TMP_LIMIT43_OUT := LIMIT(water_heigh_MIN, inner_water_height_2, water_height_MAX);
  water_height_2 := _TMP_LIMIT43_OUT;
  _TMP_BOOL_TO_REAL63_OUT := BOOL_TO_REAL(Valve_3);
  _TMP_MUL50_OUT := MUL(_TMP_BOOL_TO_REAL63_OUT, inner_Valve_3_rate);
  _TMP_SUB62_OUT := SUB(inner_water_height_2, _TMP_MUL50_OUT);
  inner_water_height_2 := _TMP_SUB62_OUT;
END_PROGRAM

PROGRAM program1
  VAR
    PID1 : PID;
  END_VAR
  VAR
    PID1_Auto AT %IX0.5 : BOOL;
  END_VAR
  VAR
    PID1_X0 : REAL := 0.3;
    PID1_KP : REAL := 0.5;
    PID1_TR : REAL := 1.3;
    PID1_TD : REAL := 1.0;
    PID1_CYCLE : TIME := T#10ms;
  END_VAR
  VAR_EXTERNAL
    water_height_2 : REAL;
    Valve_3_rate : REAL;
  END_VAR
  VAR
    setpoint_2 AT %MD10 : REAL := 30.0;
  END_VAR

  PID1(AUTO := NOT(PID1_Auto), PV := water_height_2, SP := setpoint_2, X0 := PID1_X0, KP := PID1_KP, TR := PID1_TR, TD := PID1_TD, CYCLE := PID1_CYCLE);
  Valve_3_rate := PID1.XOUT;
END_PROGRAM

PROGRAM program2
  VAR
    PID0 : PID;
  END_VAR
  VAR
    PID0_Auto AT %IX0.4 : BOOL;
  END_VAR
  VAR
    PID0_X0 : REAL := 0.3;
    PID0_KP : REAL := -0.3;
    PID0_TR : REAL := 1.3;
    PID0_TD : REAL := 1.0;
    PID0_CYCLE : TIME := T#10ms;
  END_VAR
  VAR_EXTERNAL
    water_height_1 : REAL;
    Valve_1_rate : REAL;
  END_VAR
  VAR
    setpoint_1 AT %MD8 : REAL := 50.0;
  END_VAR

  PID0(AUTO := NOT(PID0_Auto), PV := water_height_1, SP := setpoint_1, X0 := PID0_X0, KP := PID0_KP, TR := PID0_TR, TD := PID0_TD, CYCLE := PID0_CYCLE);
  Valve_1_rate := PID0.XOUT;
END_PROGRAM


CONFIGURATION Config0

  RESOURCE Res0 ON PLC
    VAR_GLOBAL
      water_height_1 AT %MD0 : REAL := 100.0;
      water_height_2 AT %MD1 : REAL := 100.0;
      Valve_1_rate AT %MD4 : REAL := 0.0;
      Valve_3_rate AT %MD5 : REAL := 0.0;
    END_VAR
    TASK task0(INTERVAL := T#200ms,PRIORITY := 0);
    TASK task2(INTERVAL := T#200ms,PRIORITY := 0);
    TASK task1(INTERVAL := T#200ms,PRIORITY := 0);
    PROGRAM instance0 WITH task0 : program0;
    PROGRAM instance2 WITH task2 : program2;
    PROGRAM instance1 WITH task1 : program1;
  END_RESOURCE
END_CONFIGURATION
