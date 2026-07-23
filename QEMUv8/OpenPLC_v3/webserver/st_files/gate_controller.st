PROGRAM Gate_Controller
  VAR
    Start_Loop : BOOL := False;
    one_hour : TIME := T#1h;
    One_Hour_Timer : TON;
    Gate_Open : BOOL := False;
  END_VAR
  VAR_OUTPUT
    Close_Gate_Inst : BOOL := False;
  END_VAR
  VAR_INPUT
    Sensed_Car : BOOL := False;
    Sensed_Car_Inst : BOOL := False;
    Sensed_App : BOOL := False;
    Mobile_Inst : BOOL := False;
    Auth : STRING;
  END_VAR
  VAR
    my_defined_str : STRING := '"1234"';
  END_VAR

  One_Hour_Timer(IN := NOT(Start_Loop), PT := one_hour);

  IF One_Hour_Timer.Q THEN
    IF Gate_Open THEN
      Close_Gate_Inst := True;
      Gate_Open := False;
    END_IF;
  END_IF;

  IF Sensed_App THEN
    IF EQ(Auth, my_defined_str) THEN
      Close_Gate_Inst := XOR(Mobile_Inst, Gate_Open);
      Gate_Open := Mobile_Inst;
    END_IF;
  END_IF;

  IF Sensed_Car THEN
    IF EQ(Auth, my_defined_str) THEN
      Close_Gate_Inst := XOR(Sensed_Car_Inst, Gate_Open);
      Gate_Open := Sensed_Car_Inst;
    END_IF;
  END_IF;


  Start_Loop := One_Hour_Timer.Q;
END_PROGRAM


CONFIGURATION Config0

  RESOURCE Res0 ON PLC
    TASK task0(INTERVAL := T#20ms,PRIORITY := 0);
    PROGRAM instance0 WITH task0 : Gate_Controller;
  END_RESOURCE
END_CONFIGURATION
