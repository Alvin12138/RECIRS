PROGRAM program0
  VAR
    Heater_enable AT %IX0.1 : BOOL;
  END_VAR
  VAR
    Heater_power : REAL := 0.0;
    Heater_prop : REAL := 0.3;
    Heater_delay : TIME := T#1s;
  END_VAR
  VAR
    Cooler_enable AT %IX0.2 : BOOL;
  END_VAR
  VAR
    Cooler_power : REAL := 0.0;
    Cooler_prop : REAL := -0.1;
    Cooler_delay : TIME := T#1s;
    Fan_enable : BOOL;
    Fan_power : REAL := 0.0;
    Fan_prop : REAL := 0.2;
  END_VAR
  VAR_EXTERNAL
    temperature : REAL;
    temperature_setpoint : REAL;
  END_VAR
  VAR
    temperature_devi : REAL := 0.1;
    Temperature_MIN : REAL := 0.0;
    Temperature_MAX : REAL := 100.0;
    inner_temperature : REAL := 19.0;
    outer_temperature : REAL := 19.0;
    inout_temperature_prop : REAL := 0.001;
  END_VAR
  VAR_EXTERNAL
    humidity : REAL;
  END_VAR
  VAR
    inner_humidity : REAL := 0.45;
    outer_humidity : REAL := 0.35;
  END_VAR
  VAR_EXTERNAL
    humidity_setpoint : REAL;
  END_VAR
  VAR
    humidity_devi : REAL := 0.01;
    humidity_MIN : REAL := 0.0;
    humidity_MAX : REAL := 1.0;
    inout_humidity_prop : REAL := 0.001;
  END_VAR
  VAR_EXTERNAL
    PID_enable : BOOL;
    PID1_enable : BOOL;
    PID_out : REAL;
    PID1_out : REAL;
  END_VAR
  VAR
    PID_zero : REAL := 0.0;
    TON0 : TON;
    TON1 : TON;
    _TMP_ADD87_OUT : REAL;
    _TMP_GT84_OUT : BOOL;
    _TMP_SUB90_OUT : REAL;
    _TMP_LT89_OUT : BOOL;
    _TMP_LIMIT52_OUT : REAL;
    _TMP_LIMIT124_OUT : REAL;
    _TMP_GT72_OUT : BOOL;
    _TMP_EQ65_OUT : BOOL;
    _TMP_LT71_OUT : BOOL;
    _TMP_ADD33_OUT : REAL;
    _TMP_LT29_OUT : BOOL;
    _TMP_SUB38_OUT : REAL;
    _TMP_GT35_OUT : BOOL;
    _TMP_AND91_OUT : BOOL;
    _TMP_MUL6_OUT : REAL;
    _TMP_MUL9_OUT : REAL;
    _TMP_GT93_OUT : BOOL;
    _TMP_BOOL_TO_REAL47_OUT : REAL;
    _TMP_MUL48_OUT : REAL;
    _TMP_ADD49_OUT : REAL;
    _TMP_LT73_OUT : BOOL;
    _TMP_BOOL_TO_REAL58_OUT : REAL;
    _TMP_MUL59_OUT : REAL;
    _TMP_SUB61_OUT : REAL;
    _TMP_BOOL_TO_REAL41_OUT : REAL;
    _TMP_MUL17_OUT : REAL;
    _TMP_ADD40_OUT : REAL;
    _TMP_BOOL_TO_REAL12_OUT : REAL;
    _TMP_MUL23_OUT : REAL;
    _TMP_SUB22_OUT : REAL;
    _TMP_GT108_OUT : BOOL;
    _TMP_BOOL_TO_REAL74_OUT : REAL;
    _TMP_MUL94_OUT : REAL;
    _TMP_ADD96_OUT : REAL;
    _TMP_EQ112_OUT : BOOL;
    _TMP_LT105_OUT : BOOL;
    _TMP_BOOL_TO_REAL100_OUT : REAL;
    _TMP_MUL101_OUT : REAL;
    _TMP_SUB102_OUT : REAL;
    _TMP_MUL118_OUT : REAL;
    _TMP_ADD120_OUT : REAL;
    _TMP_SUB139_OUT : REAL;
    _TMP_LT134_OUT : BOOL;
    _TMP_ADD133_OUT : REAL;
    _TMP_GT131_OUT : BOOL;
    _TMP_ADD147_OUT : REAL;
    _TMP_LT143_OUT : BOOL;
    _TMP_SUB151_OUT : REAL;
    _TMP_GT155_OUT : BOOL;
    _TMP_AND152_OUT : BOOL;
  END_VAR

  _TMP_ADD87_OUT := ADD(temperature_setpoint, temperature_devi);
  _TMP_GT84_OUT := GT(temperature, _TMP_ADD87_OUT);
  _TMP_SUB90_OUT := SUB(temperature_setpoint, temperature_devi);
  _TMP_LT89_OUT := LT(temperature, _TMP_SUB90_OUT);
  IF _TMP_GT84_OUT OR _TMP_LT89_OUT THEN
    PID_enable := TRUE; (*set*)
  END_IF;
  _TMP_LIMIT52_OUT := LIMIT(Temperature_MIN, inner_temperature, Temperature_MAX);
  temperature := _TMP_LIMIT52_OUT;
  IF NOT(PID_enable) THEN
    Heater_enable := FALSE; (*reset*)
  END_IF;
  IF NOT(PID_enable) THEN
    Cooler_enable := FALSE; (*reset*)
  END_IF;
  _TMP_LIMIT124_OUT := LIMIT(humidity_MIN, inner_humidity, humidity_MAX);
  humidity := _TMP_LIMIT124_OUT;
  _TMP_GT72_OUT := GT(PID_out, PID_zero);
  IF _TMP_GT72_OUT THEN
    Heater_enable := TRUE; (*set*)
  END_IF;
  _TMP_EQ65_OUT := EQ(PID_out, PID_zero);
  _TMP_LT71_OUT := LT(PID_out, PID_zero);
  IF _TMP_EQ65_OUT OR _TMP_LT71_OUT THEN
    Heater_enable := FALSE; (*reset*)
  END_IF;
  _TMP_ADD33_OUT := ADD(temperature_setpoint, temperature_devi);
  _TMP_LT29_OUT := LT(temperature, _TMP_ADD33_OUT);
  _TMP_SUB38_OUT := SUB(temperature_setpoint, temperature_devi);
  _TMP_GT35_OUT := GT(temperature, _TMP_SUB38_OUT);
  _TMP_AND91_OUT := AND(_TMP_LT29_OUT, _TMP_GT35_OUT);
  IF _TMP_AND91_OUT THEN
    PID_enable := FALSE; (*reset*)
  END_IF;
  IF _TMP_EQ65_OUT OR _TMP_GT72_OUT THEN
    Cooler_enable := FALSE; (*reset*)
  END_IF;
  IF _TMP_LT71_OUT THEN
    Cooler_enable := TRUE; (*set*)
  END_IF;
  _TMP_MUL6_OUT := MUL(PID_out, Heater_prop);
  Heater_power := _TMP_MUL6_OUT;
  _TMP_MUL9_OUT := MUL(PID_out, Cooler_prop);
  Cooler_power := _TMP_MUL9_OUT;
  _TMP_GT93_OUT := GT(outer_temperature, inner_temperature);
  _TMP_BOOL_TO_REAL47_OUT := BOOL_TO_REAL(_TMP_GT93_OUT);
  _TMP_MUL48_OUT := MUL(_TMP_BOOL_TO_REAL47_OUT, inout_temperature_prop);
  _TMP_ADD49_OUT := ADD(_TMP_MUL48_OUT, inner_temperature);
  inner_temperature := _TMP_ADD49_OUT;
  _TMP_LT73_OUT := LT(outer_temperature, inner_temperature);
  _TMP_BOOL_TO_REAL58_OUT := BOOL_TO_REAL(_TMP_LT73_OUT);
  _TMP_MUL59_OUT := MUL(_TMP_BOOL_TO_REAL58_OUT, inout_temperature_prop);
  _TMP_SUB61_OUT := SUB(inner_temperature, _TMP_MUL59_OUT);
  inner_temperature := _TMP_SUB61_OUT;
  TON0(IN := Heater_enable, PT := Heater_delay);
  _TMP_BOOL_TO_REAL41_OUT := BOOL_TO_REAL(TON0.Q);
  _TMP_MUL17_OUT := MUL(_TMP_BOOL_TO_REAL41_OUT, Heater_power);
  _TMP_ADD40_OUT := ADD(_TMP_MUL17_OUT, inner_temperature);
  inner_temperature := _TMP_ADD40_OUT;
  TON1(IN := Cooler_enable, PT := Cooler_delay);
  _TMP_BOOL_TO_REAL12_OUT := BOOL_TO_REAL(TON1.Q);
  _TMP_MUL23_OUT := MUL(_TMP_BOOL_TO_REAL12_OUT, Cooler_power);
  _TMP_SUB22_OUT := SUB(inner_temperature, _TMP_MUL23_OUT);
  inner_temperature := _TMP_SUB22_OUT;
  _TMP_GT108_OUT := GT(outer_humidity, inner_humidity);
  _TMP_BOOL_TO_REAL74_OUT := BOOL_TO_REAL(_TMP_GT108_OUT);
  _TMP_MUL94_OUT := MUL(_TMP_BOOL_TO_REAL74_OUT, inout_humidity_prop);
  _TMP_ADD96_OUT := ADD(_TMP_MUL94_OUT, inner_humidity);
  inner_humidity := _TMP_ADD96_OUT;
  _TMP_EQ112_OUT := EQ(PID1_out, PID_zero);
  IF _TMP_EQ112_OUT THEN
    Fan_enable := FALSE; (*reset*)
  END_IF;
  IF PID1_enable THEN
    Fan_enable := TRUE; (*set*)
  END_IF;
  _TMP_LT105_OUT := LT(outer_humidity, inner_humidity);
  _TMP_BOOL_TO_REAL100_OUT := BOOL_TO_REAL(_TMP_LT105_OUT);
  _TMP_MUL101_OUT := MUL(_TMP_BOOL_TO_REAL100_OUT, inout_humidity_prop);
  _TMP_SUB102_OUT := SUB(inner_humidity, _TMP_MUL101_OUT);
  inner_humidity := _TMP_SUB102_OUT;
  _TMP_MUL118_OUT := MUL(PID1_out, Fan_prop);
  Fan_power := _TMP_MUL118_OUT;
  _TMP_ADD120_OUT := ADD(inner_humidity, Fan_power);
  inner_humidity := _TMP_ADD120_OUT;
  _TMP_SUB139_OUT := SUB(humidity_setpoint, humidity_devi);
  _TMP_LT134_OUT := LT(temperature, _TMP_SUB139_OUT);
  _TMP_ADD133_OUT := ADD(humidity_setpoint, humidity_devi);
  _TMP_GT131_OUT := GT(humidity, _TMP_ADD133_OUT);
  IF _TMP_GT131_OUT OR _TMP_LT134_OUT THEN
    PID1_enable := TRUE; (*set*)
  END_IF;
  _TMP_ADD147_OUT := ADD(humidity_setpoint, humidity_devi);
  _TMP_LT143_OUT := LT(humidity, _TMP_ADD147_OUT);
  _TMP_SUB151_OUT := SUB(humidity_setpoint, humidity_devi);
  _TMP_GT155_OUT := GT(humidity, _TMP_SUB151_OUT);
  _TMP_AND152_OUT := AND(_TMP_LT143_OUT, _TMP_GT155_OUT);
  IF _TMP_AND152_OUT THEN
    PID1_enable := FALSE; (*reset*)
  END_IF;
END_PROGRAM

PROGRAM program1
  VAR
    PID0 : PID;
  END_VAR
  VAR_EXTERNAL
    PID_enable : BOOL;
  END_VAR
  VAR
    PID_X0 : REAL;
    PID_KP : REAL := -0.05;
    PID_TR : REAL := 10.0;
    PID_TD : REAL := 40.0;
    PID_CYCLE : TIME := T#10ms;
  END_VAR
  VAR_EXTERNAL
    temperature : REAL;
    PID_out : REAL;
    temperature_setpoint : REAL;
  END_VAR

  PID0(AUTO := PID_enable, PV := temperature, SP := temperature_setpoint, KP := PID_KP, TR := PID_TR, TD := PID_TD, CYCLE := PID_CYCLE);
  PID_out := PID0.XOUT;
END_PROGRAM

PROGRAM program2
  VAR
    PID1 : PID;
  END_VAR
  VAR_EXTERNAL
    PID1_enable : BOOL;
  END_VAR
  VAR
    PID1_X0 : REAL;
    PID1_KP : REAL := -0.01;
    PID1_TR : REAL := 5.0;
    PID1_TD : REAL := 20.0;
    PID1_CYCLE : TIME := T#10ms;
  END_VAR
  VAR_EXTERNAL
    humidity : REAL;
    PID1_out : REAL;
    humidity_setpoint : REAL;
  END_VAR

  PID1(AUTO := PID1_enable, PV := humidity, SP := humidity_setpoint, KP := PID1_KP, TR := PID1_TR, TD := PID1_TD, CYCLE := PID1_CYCLE);
  PID1_out := PID1.XOUT;
END_PROGRAM


CONFIGURATION Config0

  RESOURCE Res0 ON PLC
    VAR_GLOBAL
      temperature AT %MD0 : REAL := 0.0;
      humidity AT %MD4 : REAL := 0.0;
      PID_out AT %MD8 : REAL := 0.0;
      PID1_out AT %MD12 : REAL := 0.0;
      PID_enable : BOOL := 1;
      PID1_enable : BOOL := 1;
      temperature_setpoint AT %MD16 : REAL := 24.0;
      humidity_setpoint AT %MD20 : REAL := 0.35;
    END_VAR
    TASK task0(INTERVAL := T#10ms,PRIORITY := 0);
    TASK task2(INTERVAL := T#10ms,PRIORITY := 0);
    TASK task1(INTERVAL := T#10ms,PRIORITY := 0);
    PROGRAM instance0 WITH task0 : program0;
    PROGRAM instance2 WITH task2 : program2;
    PROGRAM instance1 WITH task1 : program1;
  END_RESOURCE
END_CONFIGURATION
