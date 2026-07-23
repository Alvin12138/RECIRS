PROGRAM prog0
VAR
    PeakHours : BOOL;
    NonPeakHours : BOOL;
    EnergyConsumption : REAL := 0.0;
    MaxConsumption : REAL := 1000.0;
    LoadShedding : BOOL;
    Timer : TON;
    _TEMP : REAL;
    EnergyGeneration : REAL := 500.0;
    EnergyUsage : REAL := 600.0;
END_VAR

IF PeakHours THEN
    IF EnergyConsumption > MaxConsumption THEN
        LoadShedding := TRUE;
        Timer(IN := TRUE, PT := T#30m);
    ELSE
        LoadShedding := FALSE;
        Timer(IN := FALSE);
    END_IF;
ELSE
    LoadShedding := FALSE;
    Timer(IN := FALSE);
END_IF;

IF Timer.Q THEN
	LoadShedding := FALSE;
END_IF;

IF LoadShedding THEN
	_TEMP := MUL(EnergyUsage, 0.1);
	EnergyConsumption := SUB(EnergyConsumption, _TEMP);
ELSE
	_TEMP := MUL(EnergyGeneration, 0.1);
	EnergyConsumption := ADD(EnergyConsumption, _TEMP);
END_IF;


END_PROGRAM

CONFIGURATION Config0

  RESOURCE Res0 ON PLC
    TASK Main(INTERVAL := T#50ms,PRIORITY := 0);
    PROGRAM Inst0 WITH Main : prog0;
  END_RESOURCE
END_CONFIGURATION
