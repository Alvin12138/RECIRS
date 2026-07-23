PROGRAM prog0
VAR
    StartButton : BOOL := TRUE;
    StopButton : BOOL;
    ConveyorRunning : BOOL;
    PackageDetected : BOOL;
    PackageCount : INT := 0;
    MaxPackages : INT := 100;
    SealerOn : BOOL;
    EjectorOn : BOOL;
    TimerSealer : TON;
    TimerEjector : TON;
    StartTrigger : R_TRIG;
    StopTrigger : F_TRIG;
    ConveyorSpeed : REAL := 1.0;
    ConveyorPosition : REAL := 0.0;
    PackagePositon : REAL := 0.0;
	PackageLength : REAL := 0.5;
	_TEMPPOS : REAL;
END_VAR

StartTrigger(CLK := StartButton);
StopTrigger(CLK := StopButton);

IF StartButton THEN
    ConveyorRunning := TRUE;
ELSIF StopButton THEN
    ConveyorRunning := FALSE;
END_IF;

IF ConveyorRunning THEN
	_TEMPPOS := MUL(ConveyorSpeed, 0.1);
	ConveyorPosition := ADD(ConveyorPosition, _TEMPPOS);
	IF ConveyorPosition >= PackagePositon + PackageLength THEN
		PackageDetected := TRUE;
		PackagePositon := ConveyorPosition;
	ELSE
		PackageDetected := FALSE;
	END_IF;
ELSE
	PackageDetected := FALSE;
END_IF;

IF ConveyorRunning AND PackageDetected THEN
    PackageCount := PackageCount + 1;
    TimerSealer(IN := TRUE, PT := T#5s);
    SealerOn := TimerSealer.Q;
    IF TimerSealer.Q THEN
		TimerEjector(IN := TRUE, PT := T#2s);
		EjectorOn := TimerEjector.Q;
	END_IF;
    IF PackageCount >= MaxPackages THEN
        ConveyorRunning := FALSE;
    END_IF;
ELSE
    TimerSealer(IN := FALSE);
    TimerEjector(IN := FALSE);
    SealerOn := FALSE;
    EjectorOn := FALSE;
END_IF;
END_PROGRAM

CONFIGURATION Config0

  RESOURCE Res0 ON PLC
    TASK Main(INTERVAL := T#50ms,PRIORITY := 0);
    PROGRAM Inst0 WITH Main : prog0;
  END_RESOURCE
END_CONFIGURATION
