PROGRAM program0
  VAR
    start_button : BOOL;
    stop_button : BOOL;
    emergency_button : BOOL;
    proximity_sensor : BOOL;
    motor_forward : BOOL;
    motor_reverse : BOOL;
    conveyor_running : BOOL := TRUE;
    entity_present : BOOL := FALSE;
    total_types : INT := 1;
    entity_type : INT;
    count : INT := 1;
    timer : BOOL;
    TON0 : TON;
    TOF0 : TOF;
    _TMP_ADD29_OUT : INT;
    _TMP_EQ31_OUT : BOOL;
    _TMP_BOOL_TO_INT33_OUT : INT;
    _TMP_MUL35_OUT : INT;
    _TMP_SUB37_OUT : INT;
    _TMP_INT_TO_STRING39_OUT : STRING;
  END_VAR

  TON0(IN := NOT(timer), PT := T#10ms);
  TOF0(IN := TON0.Q, PT := T#10ms);
  timer := TOF0.Q;
  IF NOT entity_present AND NOT proximity_sensor AND conveyor_running THEN
    entity_present := FALSE;
  END_IF;
  IF proximity_sensor AND conveyor_running THEN
    entity_present := TRUE;
  END_IF;
  entity_type := total_types;
  IF emergency_button THEN
    motor_forward := FALSE;
  END_IF;
  IF NOT conveyor_running THEN
    motor_forward := FALSE;
  END_IF;
  IF emergency_button THEN
    motor_reverse := FALSE;
  END_IF;
  _TMP_ADD29_OUT := ADD(count, 1);
  count := _TMP_ADD29_OUT;
  IF NOT conveyor_running THEN
    motor_reverse := FALSE;
  END_IF;
  IF emergency_button THEN
    conveyor_running := FALSE;
  END_IF;
  IF emergency_button THEN
    entity_present := FALSE;
  END_IF;
  _TMP_EQ31_OUT := EQ(count, 5);
  _TMP_BOOL_TO_INT33_OUT := BOOL_TO_INT(_TMP_EQ31_OUT);
  _TMP_MUL35_OUT := MUL(_TMP_BOOL_TO_INT33_OUT, 4);
  _TMP_SUB37_OUT := SUB(count, _TMP_MUL35_OUT);
  count := _TMP_SUB37_OUT;
  IF start_button AND NOT emergency_button THEN
    conveyor_running := TRUE;
  END_IF;
  IF stop_button AND NOT emergency_button THEN
    conveyor_running := FALSE;
  END_IF;
  _TMP_INT_TO_STRING39_OUT := INT_TO_STRING(entity_type);
END_PROGRAM


CONFIGURATION Config0

  RESOURCE Res0 ON PLC
    TASK task0(INTERVAL := T#100ms,PRIORITY := 0);
    PROGRAM instance0 WITH task0 : program0;
  END_RESOURCE
END_CONFIGURATION
