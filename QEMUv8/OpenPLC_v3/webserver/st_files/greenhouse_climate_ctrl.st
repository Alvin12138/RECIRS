PROGRAM prog0
VAR
    TempSetPoint : REAL := 22.0;
    HumiditySetPoint : REAL := 60.0;
    CurrentTemp : REAL := 20.0;
    CurrentHumidity : REAL := 55.0;
    HeaterOn : BOOL;
    CoolerOn : BOOL;
    HumidifierOn : BOOL;
    DehumidifierOn : BOOL;
    VentilationOn : BOOL;
    PIDTemp : PID;
    PIDHumidity : PID;
    _TEMP_HEAT : REAL;
    _TEMP_HUMID : REAL;
    TempChangeRate : REAL := 0.1;
    HumidityChangeRate : REAL := 0.2;
END_VAR

PIDTemp(KP := 2.0, TR := 0.5, TD := 0.1, SP := TempSetPoint, PV := CurrentTemp);
PIDHumidity(KP := 1.5, TR := 0.3, TD := 0.1, SP := HumiditySetPoint, PV := CurrentHumidity);
	

IF PIDTemp.XOUT > 0.0 THEN
    HeaterOn := TRUE;
    CoolerOn := FALSE;
ELSE
    HeaterOn := FALSE;
    CoolerOn := TRUE;
END_IF;

IF PIDHumidity.XOUT > 0.0 THEN
    HumidifierOn := TRUE;
    DehumidifierOn := FALSE;
ELSE
    HumidifierOn := FALSE;
    DehumidifierOn := FALSE;
END_IF;

IF (CurrentTemp > TempSetPoint + 2.0) OR (CurrentHumidity > HumiditySetPoint + 10.0) THEN
    VentilationOn := TRUE;
ELSE
    VentilationOn := FALSE;
END_IF;


IF HeaterOn THEN
	_TEMP_HEAT := MUL(TempChangeRate, 0.1);
	CurrentTemp := ADD(CurrentTemp, _TEMP_HEAT);
ELSIF CoolerOn THEN
	_TEMP_HEAT := MUL(TempChangeRate, 0.1);
	CurrentTemp := SUB(CurrentTemp, _TEMP_HEAT);
END_IF;

IF HumidifierOn THEN
	_TEMP_HUMID := MUL(HumidityChangeRate, 0.1);
	CurrentHumidity := ADD(CurrentHumidity, _TEMP_HUMID);
ELSIF DehumidifierOn THEN
	_TEMP_HUMID := MUL(HumidityChangeRate, 0.1);
	CurrentHumidity := SUB(CurrentHumidity, _TEMP_HUMID);
END_IF;

END_PROGRAM


CONFIGURATION Config0

  RESOURCE Res0 ON PLC
    TASK Main(INTERVAL := T#50ms,PRIORITY := 0);
    PROGRAM Inst0 WITH Main : prog0;
  END_RESOURCE
END_CONFIGURATION
