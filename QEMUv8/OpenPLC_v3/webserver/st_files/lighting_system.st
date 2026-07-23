PROGRAM prog0
VAR
    MotionDetected : BOOL;
    LightLevel : REAL;
    LightThreshold : REAL := 300.0;
    LightOn : BOOL;
    Timer : TON;
    MotionTrigger : R_TRIG;
    _TEMP : REAL;
    LightDecayRate : REAL := 10.0;
END_VAR

MotionTrigger(CLK := MotionDetected);

IF MotionTrigger.Q THEN
    LightOn := TRUE;
    Timer(IN := TRUE, PT := T#5m);
ELSE
    Timer(IN := FALSE);
END_IF;

IF Timer.Q THEN
    LightOn := FALSE;
END_IF;

IF LightLevel < LightThreshold THEN
    LightOn := TRUE;
ELSE
    LightOn := FALSE;
END_IF;

IF LightOn THEN
	_TEMP := MUL(LightDecayRate, 0.1);
	LightLevel := ADD(LightLevel, _TEMP);
ELSE
	_TEMP := MUL(LightDecayRate, 0.1);
	LightLevel := SUB(LightLevel, _TEMP);
END_IF;

END_PROGRAM

CONFIGURATION Config0

  RESOURCE Res0 ON PLC
    TASK Main(INTERVAL := T#50ms,PRIORITY := 0);
    PROGRAM Inst0 WITH Main : prog0;
  END_RESOURCE
END_CONFIGURATION
