# Diagram sources

`exploit-chain.html` is the editable Diagram Design source. The matching SVG is embedded by the Markdown documents and contains no remote font request.

Regenerate it after editing the HTML:

```sh
python3 /path/to/diagram-design/scripts/export_svg.py \
  docs/exploit-chain.html docs/exploit-chain.svg \
  --system-fonts
```

Commit both files together.
