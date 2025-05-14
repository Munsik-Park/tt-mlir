# tt-mlir 저장소 분석 리포트

## 프로젝트 개요
- **프로젝트명**: tt-mlir
- **목적**: Tenstorrent AI 가속기를 위한 MLIR 다이얼렉트 정의
- **기반**: MLIR (Multi-Level Intermediate Representation)
- **타겟**: TTNN (tt-metal)

## 프로젝트 목표
- **Generality**: 다양한 AI 모델 및 워크로드 지원 (훈련 포함)
- **Scalable**: 멀티칩 시스템 확장을 위한 퍼스트 클래스 프리미티브
- **Performant**: 뛰어난 기본 성능 제공
- **Tooling**: 인간-컴파일러 협업 최적화 지원
- **Open source**: 모든 개발 과정 공개

## 주요 디렉토리 구조
- `lib/` - 핵심 C++ 라이브러리 및 MLIR 다이얼렉트
- `python/` - Python 바인딩 및 모듈
- `tools/` - 커맨드라인 도구 (ttmlir-opt, ttmlir-translate 등)
- `test/` - 테스트 스위트
- `docs/` - 프로젝트 문서

## 빌드 시스템
- CMake 기반 (최소 3.20.0)
- Clang/Clang++ 권장
- 환경 활성화 필요: `source env/activate`

## 참고 자료
- [공식 문서](https://tenstorrent.github.io/tt-mlir/)
- [Getting Started](https://docs.tenstorrent.com/tt-mlir/getting-started.html)

---
*분석일: $(date)*
