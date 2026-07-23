PROGRAM prog0
VAR
    SolarPanelOutput : REAL := 500.0;
    BatteryLevel : REAL := 50.0;
    GridSupply : REAL := 0.0;
    LoadDemand : REAL := 600.0;
    BatteryCharging : BOOL;
    GridSupplyOn : BOOL;
    Timer : TON;
    _TEMP : REAL;
    BatteryDischargeRate : REAL := 10.0;
END_VAR

IF SolarPanelOutput > LoadDemand THEN
    BatteryCharging := TRUE;
    GridSupplyOn := FALSE;
    Timer(IN := TRUE, PT := T#1h);
ELSE
    BatteryCharging := FALSE;
    Timer(IN := FALSE);
    IF BatteryLevel < 20.0 THEN
        GridSupplyOn := TRUE;
    ELSE
        GridSupplyOn := FALSE;
    END_IF;
END_IF;

IF Timer.Q THEN
	BatteryCharging := FALSE;
END_IF;

IF BatteryCharging THEN
	_TEMP := MUL(BatteryDischargeRate, 0.1);
	BatteryLevel := ADD(BatteryLevel, _TEMP);
ELSE
	_TEMP := MUL(BatteryDischargeRate, 0.1);
	BatteryLevel := SUB(BatteryLevel, _TEMP);
END_IF;
END_PROGRAM

CONFIGURATION Config0

  RESOURCE Res0 ON PLC
    TASK Main(INTERVAL := T#50ms,PRIORITY := 0);
    PROGRAM Inst0 WITH Main : prog0;
  END_RESOURCE
END_CONFIGURATION
