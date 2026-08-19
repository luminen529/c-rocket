# C언어로 재현하는 Ariane 5 사고 시뮬레이터

Ariane 5 Flight 501 사고에서 확인된 형변환 실패와 오류 전파를 C언어로 단순화한 로켓 시뮬레이터입니다.
실제 로켓의 물리 모델이나 소프트웨어를 구현하는게 아닌 조사보고서에 기록된 핵심 사건의 순서와 작은 자료형 오류가 센서, 항법, 제어 단계로 전파되는 과정을 비교합니다.

[시뮬레이션 페이지](https://luminen529.github.io/c-rocket/)

[더 많은 정보 읽기](https://luminen529.github.io/c-rocket/info.html)

- `UNSAFE`: 두 관성 기준 장치(SRI)가 같은 형변환 오류로 차례로 정지하고, 진단 데이터가 비행 데이터로 해석되어 제어 상실과 기체 파괴로 이어진다.
- `SAFE`: 같은 정렬 계산을 실행하되 16비트 범위를 벗어난 형변환을 차단하고 정상 비행을 계속한다.(예외 처리를 추가)

## 출력 결과

1. Console log
    
    `main.c`를 실행한 터미널의 log로 결과가 표시됩니다.
   이 프로젝트는 Cmake로 컴파일을 관리하며 실행합니다.

2. CSV

    `main.c`를 실행한 경로에서 `output`폴더에 `unsafe`와 `safe`의 결과를 저장합니다.

## 코드의 핵심

실제 SRI의 Ada 코드는 보호되지 않은 변환에서 `Operand Error`를 발생시켰습니다. C에서 범위를 벗어난 실수를 작은 정수형으로 직접 변환하면 정의되지 않은 동작이 되므로, 이 프로그램은 오류 결과를 명시적으로 모델링합니다.

```c
ConversionResult convert_bias_unsafe(double value, int16_t *result)
{
    if (value < (double)INT16_MIN || value > (double)INT16_MAX) {
        return CONVERSION_OPERAND_ERROR;
    }
    *result = (int16_t)value;
    return CONVERSION_OK;
}

ConversionResult convert_bias_safe(double value, int16_t *result)
{
    if (value < (double)INT16_MIN || value > (double)INT16_MAX) {
        return CONVERSION_BLOCKED;
    }
    *result = (int16_t)value;
    return CONVERSION_OK;
}
```

따라서 실제 정의되지 않은 동작에 의존하지 않으면서도 UNSAFE의 프로세서 정지와 SAFE의 방어 동작을 항상 같은 방식으로 확인할 수 있습니다.

시뮬레이션에서는 다음 상수를 사용합니다.

```text
horizontal_velocity(T+36) = 1080
horizontal_bias(T+36)     = 32400  → 변환 성공

horizontal_velocity(T+37) = 1110
horizontal_bias(T+37)     = 33300  → UNSAFE: Operand Error / SAFE: BLOCKED
```

이 수치와 단위는 실제 아리안 5의 비행 데이터를 뜻하지 않습니다. 오류 발생 시점과 약 3.5km였던 당시 고도를 알아보기 쉽게 보여주기 위한 값입니다.

## 아리안 5 Flight 501과의 관계

1996년 6월 4일 아리안 5의 첫 시험 비행은 약 37초 뒤 유도 및 자세 정보를 잃었습니다. 아리안 4에서 재사용한 관성 기준 장치의 정렬 기능이 아리안 5에서는 이륙 후 필요하지 않았지만 계속 실행되었고, 수평 방향과 관련된 내부값을 64비트 부동소수점에서 16비트 부호 있는 정수로 변환하는 과정에서 표현 범위를 넘었습니다.

백업 SRI 1은 활성 SRI 2보다 한 데이터 주기인 72ms 먼저 같은 원인으로 정지했습니다. 따라서 활성 장치가 정지했을 때 백업 전환이 불가능했고, SRI 2의 진단 비트 패턴 일부가 비행 데이터로 해석되어 노즐이 끝까지 편향되었습니다. 약 T+39에는 20도를 넘은 받음각으로 기체가 파괴되고 자동폭파 장치가 작동했습니다.

실제 관성 기준 장치의 소프트웨어는 C가 아니라 Ada로 작성되었습니다. 이 프로젝트는 해당 사고의 복제본이 아니라, C도 임베디드 시스템에서 널리 사용된다는 점에 착안해 비슷한 형변환 실패와 안전 처리의 차이를 C로 표현한 것입니다.

참고 자료:

- [How Not To Code — A space error: $370 million for an integer overflow](https://hownot2code.wordpress.com/2016/09/02/a-space-error-370-million-for-an-integer-overflow/)
- [YouTube — Ariane 5 Flight 501, 4 June 1996](https://www.youtube.com/watch?v=N6PWATvLQCY)
- [ESA — Flight 501 failure: first information](https://sci.esa.int/web/cluster/-/36899-pr-20-1996-flight-501-failure-first-information)
- [ESA — Learning from Flight 501 and Preparing for 502](https://www.esa.int/esapub/bulletin/bullet89/dalma89.htm)
- [ESA — The Inquiry Board's Recommendations](https://www.esa.int/esapub/bulletin/bullet89/recom89.htm)
- [Ariane 501 Inquiry Board Report 사본](https://www.mssl.ucl.ac.uk/www_plasma/missions/cluster/about_cluster/cluster1/ariane5rep.php)