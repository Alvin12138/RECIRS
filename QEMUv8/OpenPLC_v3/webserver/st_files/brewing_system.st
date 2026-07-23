PROGRAM prog0
VAR
    StartButton : BOOL;
    StopButton : BOOL;
    Brewing : BOOL;
    Heating : BOOL;
    Cooling : BOOL;
    Fermenting : BOOL;
    TimerHeat : TON;
    TimerCool : TON;
    TimerFerment : TON;
    StartTrigger : R_TRIG;
    StopTrigger : F_TRIG;
    TempSetPoint : REAL := 65.0;
    _TEMPCurrent : REAL;
    CurrentTemp : REAL := 20.0;
    TempChangeRate : REAL := 0.5;
END_VAR

IF CurrentTemp < TempSetPoint THEN
	StartButton := TRUE;
END_IF;

StartTrigger(CLK := StartButton);
StopTrigger(CLK := StopButton);

IF StartTrigger.Q THEN
    Brewing := TRUE;
    TimerHeat(IN := TRUE, PT := T#1h);
    Heating := TimerHeat.Q;
    IF TimerHeat.Q THEN
        TimerCool(IN := TRUE, PT := T#30m);
        Cooling := TimerCool.Q;
    END_IF;
    IF TimerCool.Q THEN
        TimerFerment(IN := TRUE, PT := T#7d);
        Fermenting := TimerFerment.Q;
    END_IF;
ELSE
    Brewing := FALSE;
    TimerHeat(IN := FALSE);
    TimerCool(IN := FALSE);
    TimerFerment(IN := FALSE);
    Heating := FALSE;
    Cooling := FALSE;
    Fermenting := FALSE;
END_IF;

IF StopTrigger.Q THEN
    Brewing := FALSE;
    Heating := FALSE;
    Cooling := FALSE;
    Fermenting := FALSE;
END_IF;

IF Heating THEN
	_TEMPCurrent := MUL(TempChangeRate, 0.1);
	CurrentTemp := ADD(CurrentTemp, _TEMPCurrent);
ELSIF Cooling THEN
	_TEMPCurrent := MUL(TempChangeRate, 0.1);
	CurrentTemp := SUB(CurrentTemp, _TEMPCurrent);
END_IF;

END_PROGRAM

CONFIGURATION Config0

  RESOURCE Res0 ON PLC
    TASK Main(INTERVAL := T#50ms,PRIORITY := 0);
    PROGRAM Inst0 WITH Main : prog0;
  END_RESOURCE
END_CONFIGURATION
