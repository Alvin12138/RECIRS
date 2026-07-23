PROGRAM prog0

VAR
    Rdosc : BOOL;
    Cdigit : INT;
    digit : INT;
    digit0 : INT;
    digit1 : INT;
    digit2 : INT;

    Ycom_digit0 : BOOL;
    Ycom_digit1 : BOOL;
    Ycom_digit2 : BOOL;
    Yseg_a : BOOL;
    Yseg_b : BOOL;
    Yseg_c : BOOL;
    Yseg_d : BOOL;
    Yseg_e : BOOL;
    Yseg_f : BOOL;
    Yseg_g : BOOL;
END_VAR

Rdosc := FALSE;

IF Rdosc THEN
    Rdosc := TRUE;
END_IF;

IF Rdosc THEN
    Cdigit := Cdigit + 1;
    IF Cdigit > 2 THEN
        Cdigit := 0;
    END_IF;
END_IF;

CASE Cdigit OF
    0:
        Ycom_digit0 := TRUE;
        Ycom_digit1 := FALSE;
        Ycom_digit2 := FALSE;
        digit := digit0;
    1:
        Ycom_digit0 := FALSE;
        Ycom_digit1 := TRUE;
        Ycom_digit2 := FALSE;
        digit := digit1;
    2:
        Ycom_digit0 := FALSE;
        Ycom_digit1 := FALSE;
        Ycom_digit2 := TRUE;
        digit := digit2;
END_CASE;

Yseg_a := (digit <> 1) AND (digit <> 4);
Yseg_b := (digit <> 5) AND (digit <> 6);
Yseg_c := (digit <> 2);
Yseg_d := (digit <> 1) AND (digit <> 4) AND (digit <> 7);
Yseg_e := (digit = 0) OR (digit = 2) OR (digit = 6) OR (digit = 8);
Yseg_f := (digit <> 1) AND (digit <> 2) AND (digit <> 3) AND (digit <> 7);
Yseg_g := (digit <> 0) AND (digit <> 1) AND (digit <> 7);

digit0 := 1;
digit1 := 8;
digit2 := 5;

END_PROGRAM

CONFIGURATION Config0

  RESOURCE Res0 ON PLC
    TASK Main(INTERVAL := T#100ms,PRIORITY := 0);
    PROGRAM Inst0 WITH Main : prog0;
  END_RESOURCE
END_CONFIGURATION
