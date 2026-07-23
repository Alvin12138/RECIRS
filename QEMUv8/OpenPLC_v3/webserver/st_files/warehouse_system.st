PROGRAM prog0
VAR
    StartButton : BOOL := TRUE;
    StopButton : BOOL;
    ConveyorRunning : BOOL;
    ItemDetected : BOOL;
    StorageLocation : INT;
    ArmMoving : BOOL;
    ArmPosition : REAL;
    TargetPosition : REAL := 0.0;
    Timer : TON;
    StartTrigger : R_TRIG;
    StopTrigger : F_TRIG;
    _TEMPPOS : REAL;
    ConveyorSpeed : REAL := 1.0;
    ConveyorPosition : REAL := 0.0;
    ItemPosition : REAL := 0.0;
END_VAR

StartTrigger(CLK := StartButton);
StopTrigger(CLK := StopButton);

IF StartTrigger.Q THEN
    ConveyorRunning := TRUE;
ELSIF StopTrigger.Q THEN
    ConveyorRunning := FALSE;
END_IF;

IF ConveyorRunning THEN
	_TEMPPOS := MUL(ConveyorSpeed, 0.1);
	ConveyorPosition := ADD(ConveyorPosition, _TEMPPOS);
	IF ConveyorPosition >= ItemPosition + 1.0 THEN
		ItemDetected := TRUE;
		ItemPosition := ConveyorPosition;
	ELSE
		ItemDetected := FALSE;
	END_IF;
ELSE
	ItemDetected := FALSE;
END_IF;

IF ConveyorRunning AND ItemDetected THEN
    CASE StorageLocation OF
        1: TargetPosition := 10.0;
        2: TargetPosition := 20.0;
        3: TargetPosition := 30.0;
    ELSE
        TargetPosition := 0.0;
    END_CASE;
    ArmMoving := TRUE;
    Timer(IN := TRUE, PT := T#5s);
END_IF;

IF ArmMoving THEN
    IF ArmPosition < TargetPosition THEN
        ArmPosition := ArmPosition + 1.0;
    ELSE
        ArmMoving := FALSE;
    END_IF;
END_IF;

IF Timer.Q THEN
	ArmMoving := FALSE;
END_IF;

END_PROGRAM

CONFIGURATION Config0

  RESOURCE Res0 ON PLC
    TASK Main(INTERVAL := T#50ms,PRIORITY := 0);
    PROGRAM Inst0 WITH Main : prog0;
  END_RESOURCE
END_CONFIGURATION
