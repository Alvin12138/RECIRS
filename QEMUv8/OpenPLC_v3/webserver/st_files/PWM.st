PROGRAM program0
  VAR
    level : DINT;
    count : INT := 1;
    input_analog : REAL;
    input_analog_MIN : REAL := 0.0;
    input_analog_MAX : REAL := 3.3;
    input_analog_local : REAL := 0.0;
    _TMP_INT_TO_REAL42_OUT : REAL;
    _TMP_SIN47_OUT : REAL;
    _TMP_ADD48_OUT : REAL;
    _TMP_MUL43_OUT : REAL;
    _TMP_LIMIT35_OUT : INT;
    _TMP_LIMIT31_OUT : REAL;
    _TMP_DIV23_OUT : REAL;
    _TMP_MUL25_OUT : REAL;
    _TMP_REAL_TO_DINT27_OUT : DINT;
    _TMP_EQ29_OUT : BOOL;
    _TMP_BOOL_TO_INT44_OUT : INT;
    _TMP_MUL45_OUT : INT;
    _TMP_SUB46_OUT : INT;
    _TMP_ADD19_OUT : INT;
  END_VAR

  _TMP_INT_TO_REAL42_OUT := INT_TO_REAL(count);
  _TMP_SIN47_OUT := SIN(_TMP_INT_TO_REAL42_OUT);
  _TMP_ADD48_OUT := ADD(_TMP_SIN47_OUT, 1.0);
  _TMP_MUL43_OUT := MUL(_TMP_ADD48_OUT, 1.65);
  input_analog := _TMP_MUL43_OUT;
  _TMP_LIMIT35_OUT := LIMIT(1, count, 200);
  count := _TMP_LIMIT35_OUT;
  _TMP_LIMIT31_OUT := LIMIT(input_analog_MIN, input_analog, input_analog_MAX);
  input_analog_local := _TMP_LIMIT31_OUT;
  _TMP_DIV23_OUT := DIV(input_analog_local, input_analog_MAX);
  _TMP_MUL25_OUT := MUL(_TMP_DIV23_OUT, 65535.0);
  _TMP_REAL_TO_DINT27_OUT := REAL_TO_DINT(_TMP_MUL25_OUT);
  level := _TMP_REAL_TO_DINT27_OUT;
  _TMP_EQ29_OUT := EQ(count, 200);
  _TMP_BOOL_TO_INT44_OUT := BOOL_TO_INT(_TMP_EQ29_OUT);
  _TMP_MUL45_OUT := MUL(_TMP_BOOL_TO_INT44_OUT, 200);
  _TMP_SUB46_OUT := SUB(count, _TMP_MUL45_OUT);
  count := _TMP_SUB46_OUT;
  _TMP_ADD19_OUT := ADD(count, 1);
  count := _TMP_ADD19_OUT;
END_PROGRAM


CONFIGURATION Config0

  RESOURCE Res0 ON PLC
    TASK task0(INTERVAL := T#20ms,PRIORITY := 0);
    PROGRAM instance0 WITH task0 : program0;
  END_RESOURCE
END_CONFIGURATION
