"""
camera.py — Simple top-down camera with pan, zoom, and tile<->screen transforms.
"""

from __future__ import annotations
from typing import Tuple

import config


class Camera:
    """Pan, zoom, and project between tile coords and screen pixels."""

    def __init__(
        self,
        viewport_w: int,
        viewport_h: int,
        world_w_tiles: int,
        world_h_tiles: int,
    ) -> None:
        self.screen_w = viewport_w
        self.screen_h = viewport_h
        self.world_w = world_w_tiles
        self.world_h = world_h_tiles
        # Position in tile coordinates of the tile shown at the screen center.
        self.cx: float = world_w_tiles / 2.0
        self.cy: float = world_h_tiles / 2.0
        self.zoom: float = config.DEFAULT_ZOOM

    # ── Pan / zoom ────────────────────────────────────────────────────────────
    def pan(self, dx_pixels: float, dy_pixels: float) -> None:
        """Pan the view by a pixel delta (positive dx moves the world right under mouse-drag math)."""
        tw, th = self._tile_pixels()
        if tw == 0 or th == 0:
            return
        self.cx += dx_pixels / tw
        self.cy += dy_pixels / th
        self._clamp()

    def zoom_at(self, screen_pos: Tuple[int, int], delta: float) -> None:
        """Zoom toward a screen-space point by a multiplicative delta (e.g. +0.1)."""
        old_tile = self.screen_to_tile(*screen_pos)
        self.zoom = max(config.CAMERA_MIN_ZOOM, min(config.CAMERA_MAX_ZOOM, self.zoom + delta))
        new_tile = self.screen_to_tile(*screen_pos)
        # Keep the cursor on the same world tile after zoom
        self.cx += old_tile[0] - new_tile[0]
        self.cy += old_tile[1] - new_tile[1]
        self._clamp()

    def center_on(self, tile_x: float, tile_y: float) -> None:
        self.cx = float(tile_x)
        self.cy = float(tile_y)
        self._clamp()

    def resize(self, w: int, h: int) -> None:
        self.screen_w = w
        self.screen_h = h

    def _clamp(self) -> None:
        # Allow some overscroll so edges are reachable
        self.cx = max(0.0, min(float(self.world_w), self.cx))
        self.cy = max(0.0, min(float(self.world_h), self.cy))

    def _tile_pixels(self) -> Tuple[float, float]:
        return config.TILE_WIDTH * self.zoom, config.TILE_HEIGHT * self.zoom

    # ── Projection ────────────────────────────────────────────────────────────
    def tile_to_screen(self, tile_x: float, tile_y: float) -> Tuple[float, float]:
        tw, th = self._tile_pixels()
        sx = self.screen_w / 2 + (tile_x - self.cx) * tw
        sy = self.screen_h / 2 + (tile_y - self.cy) * th
        return sx, sy

    def screen_to_tile(self, sx: float, sy: float) -> Tuple[float, float]:
        """Convert a screen pixel position to FLOAT tile coordinates.

        Returns floats (not ints) so callers can do precise distance
        comparisons (e.g. AgentPanel click hit-testing). Callers that need
        an integer tile index should int() the result themselves.
        """
        tw, th = self._tile_pixels()
        if tw == 0 or th == 0:
            return 0.0, 0.0
        tx = self.cx + (sx - self.screen_w / 2) / tw
        ty = self.cy + (sy - self.screen_h / 2) / th
        return tx, ty

    def screen_to_tile_int(self, sx: float, sy: float) -> Tuple[int, int]:
        """Integer tile index under the cursor — for grid-cell queries."""
        tx, ty = self.screen_to_tile(sx, sy)
        return int(tx), int(ty)
