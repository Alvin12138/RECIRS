PROGRAM prog0
VAR
    StartButton : BOOL := TRUE;
    StopButton : BOOL;
    ArmPosition : REAL := 0.0;
    TargetPosition : REAL := 100.0;
    ArmMoving : BOOL;
    ArmSpeed : REAL := 10.0;
    Timer : TON;
    StartTrigger : R_TRIG;
    StopTrigger : F_TRIG;
    ArmAcceleration : REAL := 1.0;
    ArmVelocity : REAL := 0.0;
    _TempArmPosition : REAL;
    _TempTimer : REAL;
END_VAR



StartTrigger(CLK := StartButton);
StopTrigger(CLK := StopButton);

IF StartTrigger.Q THEN
    ArmMoving := TRUE;
    Timer(IN := TRUE, PT := T#10s);
ELSIF StopTrigger.Q THEN
    ArmMoving := FALSE;
    Timer(IN := FALSE);
END_IF;

IF ArmMoving THEN
    IF ArmPosition < TargetPosition THEN
		_TempTimer := TIME_TO_REAL(Timer.ET);
		_TempArmPosition := MUL(ArmSpeed, _TempTimer);
        ArmPosition := ADD(ArmPosition, _TempArmPosition);
    ELSE
        ArmMoving := FALSE;
    END_IF;
END_IF;
END_PROGRAM

CONFIGURATION Config0

  RESOURCE Res0 ON PLC
    TASK Main(INTERVAL := T#50ms,PRIORITY := 0);
    PROGRAM Inst0 WITH Main : prog0;
  END_RESOURCE
END_CONFIGURATION
