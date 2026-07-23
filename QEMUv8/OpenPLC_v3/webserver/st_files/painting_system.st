PROGRAM prog0
VAR
    StartButton : BOOL;
    StopButton : BOOL;
    Painting : BOOL;
    PaintLevel : REAL := 0.0;
    PaintThreshold : REAL := 10.0;
    _TEMPPAINT : REAL;
    RefillPaint : BOOL;
    Timer : TON;
    StartTrigger : R_TRIG;
    StopTrigger : F_TRIG;
    PaintUsageRate : REAL := 1.0;
END_VAR

StartTrigger(CLK := StartButton);
StopTrigger(CLK := StopButton);

IF StartTrigger.Q THEN
    Painting := TRUE;
    Timer(IN := TRUE, PT := T#10m);
ELSIF StopTrigger.Q THEN
    Painting := FALSE;
    Timer(IN := FALSE);
END_IF;


IF PaintLevel < PaintThreshold THEN
	StopButton := FALSE;
	StartButton := TRUE;
ELSIF PaintLevel = 100.0 THEN
	StopButton := TRUE;
	StartButton := FALSE;
END_IF;


IF Timer.Q THEN
    Painting := FALSE;
END_IF;

IF Painting THEN
	_TEMPPAINT := MUL(PaintUsageRate, 0.5);
	PaintLevel := ADD(PaintLevel, _TEMPPAINT);
ELSE
	_TEMPPAINT := MUL(PaintUsageRate, 0.1);
	PaintLevel := SUB(PaintLevel, _TEMPPAINT);
END_IF;


END_PROGRAM

CONFIGURATION Config0

  RESOURCE Res0 ON PLC
    TASK Main(INTERVAL := T#50ms,PRIORITY := 0);
    PROGRAM Inst0 WITH Main : prog0;
  END_RESOURCE
END_CONFIGURATION
