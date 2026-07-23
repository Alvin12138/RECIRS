PROGRAM program0
  VAR
    floor_buttons : ARRAY [1..5] OF BOOL;
    door_sensor : BOOL;
    motor_up : BOOL;
    motor_down : BOOL;
    open_door : BOOL;
    close_door : BOOL;
    current_floor : INT := 1;
    target_floor : INT := 1;
    door_opened : BOOL := FALSE;
    count : INT := 1;
  END_VAR

  IF NOT door_opened THEN
      FOR count := 1 TO 5 DO
          IF floor_buttons[count] THEN
              target_floor := count;  
          END_IF;
      END_FOR;
      IF target_floor > current_floor THEN
          motor_up := TRUE;   
      ELSIF target_floor < current_floor THEN
          motor_up := FALSE;
          motor_down := TRUE; 
      ELSE
          motor_up := FALSE;
          motor_down := FALSE;
      END_IF;
      IF target_floor = current_floor THEN
          motor_up := FALSE;
          motor_down := FALSE;
          open_door := TRUE;
          door_opened := TRUE;
      END_IF;
  ELSE
      IF NOT door_sensor THEN
          close_door := TRUE;
      ELSE
          close_door := FALSE;
          open_door := FALSE;  
          door_opened := FALSE;
          current_floor := target_floor;
      END_IF;
  END_IF;
END_PROGRAM


CONFIGURATION Config0

  RESOURCE Res0 ON PLC
    TASK task0(INTERVAL := T#1ms,PRIORITY := 0);
    PROGRAM instance0 WITH task0 : program0;
  END_RESOURCE
END_CONFIGURATION
