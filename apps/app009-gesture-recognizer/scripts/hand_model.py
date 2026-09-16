# SPDX-License-Identifier: MIT
"""Version 2 absolute appearance/geometry plus local-motion causal models."""

import torch
from torch import nn
from torchvision.models import mobilenet_v3_small, MobileNet_V3_Small_Weights

CHANNELS = {"absolute": 576, "geometry": 134, "roi": 576, "fusion": 710}


class Appearance(nn.Module):
    def __init__(self, pretrained=True):
        super().__init__()
        self.features = mobilenet_v3_small(
            weights=MobileNet_V3_Small_Weights.IMAGENET1K_V1 if pretrained else None
        ).features

    def forward(self, x):
        return self.features(x).mean((2, 3))

    def finetune(self):
        for p in self.parameters():
            p.requires_grad_(False)
        for p in self.features[-3:].parameters():
            p.requires_grad_(True)
        self.eval()


class HandTemporal(nn.Module):
    def __init__(self, branch, window=32, mean=None, std=None):
        super().__init__()
        if branch not in CHANNELS or window not in [32, 64]:
            raise ValueError("Unsupported version 2 architecture")
        channels = CHANNELS[branch]
        self.branch = branch
        self.window = window
        self.register_buffer(
            "mean",
            (
                torch.zeros(1, channels, 1, 1)
                if mean is None
                else torch.as_tensor(mean).float().reshape(1, channels, 1, 1)
            ),
        )
        self.register_buffer(
            "std",
            (
                torch.ones(1, channels, 1, 1)
                if std is None
                else torch.as_tensor(std)
                .float()
                .clamp_min(0.05)
                .reshape(1, channels, 1, 1)
            ),
        )
        self.project = nn.Conv2d(channels * 2, 128, 1)
        self.layers = nn.ModuleList(
            [nn.Conv2d(128, 128, (1, 3), dilation=(1, d)) for d in [1, 2, 4, 8, 16]]
        )
        self.dropout = nn.Dropout2d(0.2)
        self.classifier = nn.Conv2d(128, 12, 1)

    def sequence(self, x):
        z = ((x - self.mean) / self.std).clamp(-10, 10)
        previous = torch.cat([z[:, :, :, :1], z[:, :, :, :-1]], 3)
        z = torch.cat([z, z - previous], 1)
        z = self.dropout(torch.relu(self.project(z)))
        for layer, d in zip(self.layers, [1, 2, 4, 8, 16]):
            z = torch.relu(
                z + self.dropout(layer(nn.functional.pad(z, (2 * d, 0, 0, 0))))
            )
        return self.classifier(z)

    def forward(self, x):
        return self.sequence(x)[:, :, 0, -1]
