//! The viewer's input, as `desktop.input` bodies (docs/protocol.md). Positions are fractions
//! (0..1) of the shared screen.

use serde::Deserialize;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Deserialize, Default)]
#[serde(rename_all = "lowercase")]
pub enum Button {
    #[default]
    Left,
    Right,
    Middle,
}

#[derive(Debug, Clone, PartialEq, Deserialize)]
#[serde(tag = "type", rename_all = "lowercase")]
pub enum Input {
    /// The pointer moves to a point.
    Move { x: f64, y: f64 },
    /// A button goes down or up at a point (a drag: down, moves, up).
    Button {
        #[serde(default)]
        button: Button,
        down: bool,
        x: f64,
        y: f64,
    },
    /// A click (down and up) at a point; `count` 2 is a double click.
    Click {
        #[serde(default)]
        button: Button,
        x: f64,
        y: f64,
        #[serde(default = "one")]
        count: u32,
    },
    /// Wheel notches at a point: positive `dy` scrolls down, positive `dx` right.
    Scroll {
        x: f64,
        y: f64,
        #[serde(default)]
        dx: f64,
        #[serde(default)]
        dy: f64,
    },
    /// A named key (see `Key`), pressed and released unless `down` is given; `modifiers` are held
    /// around it.
    Key {
        key: String,
        #[serde(default)]
        down: Option<bool>,
        #[serde(default)]
        modifiers: Vec<String>,
    },
    /// Text typed as it is (any language).
    Text { text: String },
}

fn one() -> u32 {
    1
}

impl Input {
    pub fn parse(json: &str) -> anyhow::Result<Self> {
        let mut input: Input = serde_json::from_str(json)?;
        input.clamp();
        Ok(input)
    }

    fn clamp(&mut self) {
        let c = |v: &mut f64| {
            *v = if v.is_finite() {
                v.clamp(0.0, 1.0)
            } else {
                0.5
            }
        };
        match self {
            Input::Move { x, y } | Input::Button { x, y, .. } | Input::Click { x, y, .. } => {
                c(x);
                c(y)
            }
            Input::Scroll { x, y, dx, dy } => {
                c(x);
                c(y);
                *dx = if dx.is_finite() {
                    dx.clamp(-20.0, 20.0)
                } else {
                    0.0
                };
                *dy = if dy.is_finite() {
                    dy.clamp(-20.0, 20.0)
                } else {
                    0.0
                };
            }
            Input::Key { .. } => {}
            Input::Text { text } => {
                text.truncate(text.char_indices().nth(4096).map_or(text.len(), |(i, _)| i));
            }
        }
        if let Input::Click { count, .. } = self {
            *count = (*count).clamp(1, 3);
        }
    }
}

/// Keys a viewer may name. Each platform maps them to its own codes.
#[cfg_attr(not(test), allow(dead_code))]
pub const KEYS: &[&str] = &[
    "Enter",
    "Backspace",
    "Escape",
    "Tab",
    "Delete",
    "Space",
    "Left",
    "Right",
    "Up",
    "Down",
    "Home",
    "End",
    "PageUp",
    "PageDown",
    "Insert",
    "Shift",
    "Control",
    "Alt",
    "Super",
    "F1",
    "F2",
    "F3",
    "F4",
    "F5",
    "F6",
    "F7",
    "F8",
    "F9",
    "F10",
    "F11",
    "F12",
];

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_and_clamps() {
        assert_eq!(
            Input::parse(r#"{"type":"move","x":1.5,"y":-1}"#).unwrap(),
            Input::Move { x: 1.0, y: 0.0 }
        );
        assert_eq!(
            Input::parse(r#"{"type":"click","x":0.5,"y":0.5}"#).unwrap(),
            Input::Click {
                button: Button::Left,
                x: 0.5,
                y: 0.5,
                count: 1
            }
        );
        assert_eq!(
            Input::parse(r#"{"type":"button","button":"right","down":true,"x":0,"y":0}"#).unwrap(),
            Input::Button {
                button: Button::Right,
                down: true,
                x: 0.0,
                y: 0.0
            }
        );
        assert!(matches!(
            Input::parse(r#"{"type":"key","key":"Enter","modifiers":["Control"]}"#).unwrap(),
            Input::Key { down: None, .. }
        ));
        assert!(Input::parse(r#"{"type":"teleport"}"#).is_err());
    }
}
