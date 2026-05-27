# 데모 동영상 / GIF

이 폴더에 각 예제의 데모 파일을 두면 [../../EXAMPLES.md](../../EXAMPLES.md) 와 [../../README.md](../../README.md) 의 placeholder 가 자동으로 표시됩니다.

## 기대 파일

| 파일명 | 어디서 참조되나 |
|---|---|
| `uniflow_overview.gif` 또는 `.mp4` | [README.md](../../README.md) 의 "동영상" 섹션 |
| `cnc_pickers.gif` 또는 `.mp4` | [EXAMPLES.md](../../EXAMPLES.md) §0 |
| `shared_ostream.gif` 또는 `.mp4` | [EXAMPLES.md](../../EXAMPLES.md) §1 |
| `queue_drain.gif` 또는 `.mp4` | [EXAMPLES.md](../../EXAMPLES.md) §2 |
| `message_dispatch.gif` 또는 `.mp4` | [EXAMPLES.md](../../EXAMPLES.md) §3 |
| `weather_llm.gif` 또는 `.mp4` | [EXAMPLES.md](../../EXAMPLES.md) §4 |

## 권장 사양

- **길이**: 20~40초 (모듈 한 사이클이 한 번 보이게)
- **포맷**: `.mp4` (Github 미리보기 가능) 또는 `.gif` (작은 용량, 자동 재생)
- **해상도**: 720p~1080p, 30fps
- **녹화 도구**: ShareX (Windows), OBS, ScreenToGif

## 마크다운 안에서 표시

`.gif`는 `![제목](경로)` 한 줄로 자동 재생됩니다:
```markdown
![cnc_pickers demo](docs/videos/cnc_pickers.gif)
```

`.mp4` 는 HTML 태그가 필요:
```markdown
<video src="docs/videos/cnc_pickers.mp4" controls width="700"></video>
```
