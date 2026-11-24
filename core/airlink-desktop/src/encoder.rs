//! Captured frames to independent H.264 key frames: `ALH1` + Annex B (SPS, PPS, IDR), the format
//! the phone's screen already uses (docs/protocol.md §10). Each frame decodes on its own, so the
//! media stream's "newest frame wins" never leaves the viewer without a reference frame.

use openh264::OpenH264API;
use openh264::encoder::{BitRate, Encoder, EncoderConfig, FrameRate, UsageType};
use openh264::formats::{RgbSliceU8, YUVBuffer};

use crate::RawFrame;

/// The encoded size for a frame: at most `max_width` wide, aspect kept, both sides even.
pub fn scaled_size(width: usize, height: usize, max_width: usize) -> (usize, usize) {
    let (w, h) = if width > max_width && width > 0 {
        (max_width, height * max_width / width)
    } else {
        (width, height)
    };
    ((w & !1).max(2), (h & !1).max(2))
}

pub struct FrameEncoder {
    encoder: Encoder,
    width: usize,
    height: usize,
    rgb: Vec<u8>,
}

impl FrameEncoder {
    pub fn new(width: usize, height: usize) -> anyhow::Result<Self> {
        let config = EncoderConfig::new()
            .usage_type(UsageType::ScreenContentRealTime)
            .bitrate(BitRate::from_bps(6_000_000))
            .max_frame_rate(FrameRate::from_hz(30.0))
            .skip_frames(false);
        let encoder = Encoder::with_api_config(OpenH264API::from_source(), config)?;
        Ok(Self {
            encoder,
            width,
            height,
            rgb: vec![0; width * height * 3],
        })
    }

    pub fn size(&self) -> (usize, usize) {
        (self.width, self.height)
    }

    /// One key frame for `frame` (scaled to this encoder's size), or None on a bad frame.
    pub fn encode(&mut self, frame: &RawFrame) -> Option<Vec<u8>> {
        if frame.width == 0 || frame.height == 0 || frame.stride < frame.width * 4 {
            return None;
        }
        if frame.data.len() < frame.stride * (frame.height - 1) + frame.width * 4 {
            return None;
        }
        // Nearest-neighbour scaling into packed RGB.
        let (r, b) = if frame.rgb_order { (0, 2) } else { (2, 0) };
        for y in 0..self.height {
            let sy = y * frame.height / self.height;
            let row = &frame.data[sy * frame.stride..];
            let out = &mut self.rgb[y * self.width * 3..(y + 1) * self.width * 3];
            for x in 0..self.width {
                let sx = x * frame.width / self.width * 4;
                out[x * 3] = row[sx + r];
                out[x * 3 + 1] = row[sx + 1];
                out[x * 3 + 2] = row[sx + b];
            }
        }
        let yuv = YUVBuffer::from_rgb_source(RgbSliceU8::new(&self.rgb, (self.width, self.height)));
        self.encoder.force_intra_frame();
        let stream = self.encoder.encode(&yuv).ok()?;
        let mut packet = b"ALH1".to_vec();
        stream.write_vec(&mut packet);
        (packet.len() > 4).then_some(packet)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use openh264::decoder::Decoder;
    use openh264::formats::YUVSource;

    fn frame(w: usize, h: usize, shade: u8) -> RawFrame {
        RawFrame {
            width: w,
            height: h,
            stride: w * 4,
            rgb_order: false,
            data: vec![shade; w * h * 4],
        }
    }

    #[test]
    fn sizes_are_even_and_capped() {
        assert_eq!(scaled_size(1920, 1080, 1280), (1280, 720));
        assert_eq!(scaled_size(801, 601, 1280), (800, 600));
        assert_eq!(scaled_size(2560, 1440, 1280), (1280, 720));
    }

    #[test]
    fn every_frame_decodes_alone() {
        let mut encoder = FrameEncoder::new(64, 48).unwrap();
        let first = encoder.encode(&frame(128, 96, 40)).unwrap();
        let second = encoder.encode(&frame(128, 96, 200)).unwrap();
        for packet in [first, second] {
            assert!(packet.starts_with(b"ALH1"));
            // A fresh decoder for each: no frame depends on an earlier one.
            let mut decoder = Decoder::new().unwrap();
            let image = decoder.decode(&packet[4..]).unwrap().expect("a picture");
            assert_eq!(image.dimensions(), (64, 48));
        }
    }
}
