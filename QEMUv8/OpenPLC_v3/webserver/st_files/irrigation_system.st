PROGRAM prog0
VAR
    SoilMoisture : REAL := 40.0;
    MoistureSetPoint : REAL := 50.0;
    MoistureUpperLimit : REAL := 60.0;
    MoistureLowerLimit : REAL := 30.0;
    IrrigationOn : BOOL;
    IrrigationAddRate : REAL := 5.0;
    Timer : TON;
    MoistureDecayRate : REAL := 0.5;
END_VAR

IF SoilMoisture < MoistureLowerLimit THEN
    IrrigationOn := TRUE;
    Timer(IN := TRUE, PT := T#30m);
ELSE
    IrrigationOn := FALSE;
    Timer(IN := FALSE);
END_IF;

IF Timer.Q THEN
    IrrigationOn := FALSE;
END_IF;

IF IrrigationOn THEN
	SoilMoisture := ADD(SoilMoisture, IrrigationAddRate);
END_IF;

SoilMoisture := SUB(SoilMoisture, MoistureDecayRate);

END_PROGRAM

CONFIGURATION Config0

  RESOURCE Res0 ON PLC
    TASK Main(INTERVAL := T#1s,PRIORITY := 0);
    PROGRAM Inst0 WITH Main : prog0;
  END_RESOURCE
END_CONFIGURATION
