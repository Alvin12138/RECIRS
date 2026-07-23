PROGRAM prog0
VAR
    StartButton : BOOL;
    StopButton : BOOL;
    ConveyorRunning : BOOL;
    ItemDetected : BOOL;
    ItemType : INT;
    SortBin1 : BOOL;
    SortBin2 : BOOL;
    SortBin3 : BOOL;
    Timer : TON;
    StartTrigger : R_TRIG;
    StopTrigger : F_TRIG;
    _TEMPPOS : REAL;
    ConveyorSpeed : REAL := 1.0;
    ConveyorPosition : REAL := 0.0;
    ItemPosition : REAL := 0.0;
    Bin1Position : REAL := 2.0;
    Bin2Position : REAL := 4.0;
    Bin3Position : REAL := 6.0;
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
    CASE ItemType OF
        1: 
			SortBin1 := TRUE;
			Timer(IN := TRUE, PT := T#2s);
        2: 
			SortBin2 := TRUE;
			Timer(IN := TRUE, PT := T#2s);
        3: 
			SortBin3 := TRUE;
			Timer(IN := TRUE, PT := T#2s);
    ELSE
        SortBin1 := FALSE;
        SortBin2 := FALSE;
        SortBin3 := FALSE;
    END_CASE;
END_IF;

IF Timer.Q THEN
	SortBin1 := FALSE;
	SortBin2 := FALSE;
	SortBin3 := FALSE;
END_IF;	

END_PROGRAM

CONFIGURATION Config0

  RESOURCE Res0 ON PLC
    TASK Main(INTERVAL := T#50ms,PRIORITY := 0);
    PROGRAM Inst0 WITH Main : prog0;
  END_RESOURCE
END_CONFIGURATION
