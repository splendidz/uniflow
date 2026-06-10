# 데모 미디어 (docs/videos)

이 폴더는 예제 데모 GIF/동영상을 모읍니다.

## 현재 파일

| 파일 | 어디서 참조되나 |
|---|---|
| `city_traffic.gif` | [README](../../README.md) hero, [EXAMPLES](../../EXAMPLES.md), [city_traffic 페이지](../../examples/city_traffic/README.md) |
| `cnc_picker.gif` | [README](../../README.md) hero, [EXAMPLES](../../EXAMPLES.md), [cnc_pickers 페이지](../../examples/cnc_pickers/README.md) |

## 권장 사양

- **길이**: 20-40초 (모듈 한 사이클이 한 번 보이게)
- **포맷**: `.gif` (자동 재생, GitHub 미리보기) 또는 `.mp4`
- **해상도**: 720p-1080p, 30fps
- **녹화 도구**: ShareX (Windows), OBS, ScreenToGif

## 마크다운 안에서 표시

`.gif`는 `![제목](경로)` 한 줄로 자동 재생됩니다:

```markdown
![city_traffic demo](docs/videos/city_traffic.gif)
```

`.mp4`는 HTML 태그가 필요:

```markdown
<video src="docs/videos/city_traffic.mp4" controls width="700"></video>
```
