PROGRAM prog0
VAR
    StartButton : BOOL := TRUE;
    StopButton : BOOL;
    CarDetected : BOOL;
    Washing : BOOL;
    Rinsing : BOOL;
    Drying : BOOL;
    TimerWash : TON;
    TimerRinse : TON;
    TimerDry : TON;
    StartTrigger : R_TRIG;
    StopTrigger : F_TRIG;
    _TEMP : REAL;
    CarPosition : REAL := 0.0;
    CarSpeed : REAL := 0.5;
    WashPosition : REAL := 2.0;
    RinsePosition : REAL := 4.0;
    DryPosition : REAL := 6.0;
END_VAR

StartTrigger(CLK := StartButton);
StopTrigger(CLK := StopButton);

IF StartTrigger.Q AND CarDetected THEN
    TimerWash(IN := TRUE, PT := T#2m);
    Washing := TimerWash.Q;
    IF TimerWash.Q THEN
        TimerRinse(IN := TRUE, PT := T#1m);
        Rinsing := TimerRinse.Q;
    END_IF;
    IF TimerRinse.Q THEN
        TimerDry(IN := TRUE, PT := T#1m);
        Drying := TimerDry.Q;
    END_IF;
ELSE
    TimerWash(IN := FALSE);
    TimerRinse(IN := FALSE);
    TimerDry(IN := FALSE);
    Washing := FALSE;
    Rinsing := FALSE;
    Drying := FALSE;
END_IF;

IF StopTrigger.Q THEN
    Washing := FALSE;
    Rinsing := FALSE;
    Drying := FALSE;
END_IF;

_TEMP := MUL(CarSpeed, 0.1);
IF Washing THEN
	CarPosition := ADD(CarPosition, _TEMP);
	IF CarPosition >= WashPosition THEN
		Washing := FALSE;
		Rinsing := TRUE;
	END_IF;
END_IF;

IF Rinsing THEN
	CarPosition := ADD(CarPosition, _TEMP);
	IF CarPosition >= RinsePosition THEN
		Rinsing := FALSE;
		Drying := TRUE;
	END_IF;
END_IF;

IF Drying THEN
	CarPosition := ADD(CarPosition, _TEMP);
	IF CarPosition >= DryPosition THEN
		Drying := FALSE;
	END_IF;
END_IF;


END_PROGRAM

CONFIGURATION Config0

  RESOURCE Res0 ON PLC
    TASK Main(INTERVAL := T#50ms,PRIORITY := 0);
    PROGRAM Inst0 WITH Main : prog0;
  END_RESOURCE
END_CONFIGURATION
