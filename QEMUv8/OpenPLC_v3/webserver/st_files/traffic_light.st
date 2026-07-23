PROGRAM Trafic_light
  VAR
    TOF0 : TOF;
    Always_True : BOOL := True;
    Delay_PLC_start_up : BOOL;
    TOF1 : TOF;
    TOF2 : TOF;
  END_VAR
  VAR
    O_LEDRed AT %QX100.0 : BOOL;
    O_LEDOrange AT %QX100.1 : BOOL;
    O_LEDGreen AT %QX100.2 : BOOL;
  END_VAR
  VAR
    No_lamps_active_for_1s : BOOL;
    F_TRIG0 : F_TRIG;
    F_TRIG1 : F_TRIG;
    F_TRIG2 : F_TRIG;
    TON0 : TON;
    SR0 : SR;
    TOF3 : TOF;
    TOF4 : TOF;
    TOF5 : TOF;
    _TMP_OR22_OUT : BOOL;
    _TMP_AND29_OUT : BOOL;
    _TMP_AND33_OUT : BOOL;
    _TMP_AND34_OUT : BOOL;
    _TMP_OR13_OUT : BOOL;
    _TMP_NOT14_OUT : BOOL;
  END_VAR

  TON0(IN := Always_True, PT := T#2s);
  SR0(S1 := TON0.Q);
  Delay_PLC_start_up := SR0.Q1;
  F_TRIG2(CLK := O_LEDOrange);
  _TMP_OR22_OUT := OR(No_lamps_active_for_1s, F_TRIG2.Q);
  TOF0(IN := _TMP_OR22_OUT, PT := T#10s);
  _TMP_AND29_OUT := AND(TOF0.Q, Delay_PLC_start_up);
  O_LEDRed := _TMP_AND29_OUT;
  F_TRIG0(CLK := O_LEDGreen);
  TOF1(IN := F_TRIG0.Q, PT := T#4s);
  _TMP_AND33_OUT := AND(TOF1.Q, Delay_PLC_start_up);
  O_LEDOrange := _TMP_AND33_OUT;
  F_TRIG1(CLK := O_LEDRed);
  TOF2(IN := F_TRIG1.Q, PT := T#10s);
  _TMP_AND34_OUT := AND(TOF2.Q, Delay_PLC_start_up);
  O_LEDGreen := _TMP_AND34_OUT;
  TOF3(IN := O_LEDRed, PT := T#1s);
  TOF4(IN := O_LEDGreen, PT := T#1s);
  TOF5(IN := O_LEDOrange, PT := T#1s);
  _TMP_OR13_OUT := OR(TOF3.Q, TOF4.Q, TOF5.Q);
  _TMP_NOT14_OUT := NOT(_TMP_OR13_OUT);
  No_lamps_active_for_1s := _TMP_NOT14_OUT;
END_PROGRAM


CONFIGURATION Config0

  RESOURCE Res0 ON PLC
    TASK task0(INTERVAL := T#20ms,PRIORITY := 0);
    PROGRAM instance0 WITH task0 : Trafic_light;
  END_RESOURCE
END_CONFIGURATION
