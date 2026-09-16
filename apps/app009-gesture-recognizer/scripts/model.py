# SPDX-License-Identifier: MIT
"""Fixed-shape feature and causal temporal graphs for QNN deployment."""

import torch
from torch import nn
from torchvision.models import mobilenet_v3_small, MobileNet_V3_Small_Weights

ARCHITECTURES = {
    "global": "mobilenet-global-tcn",
    "spatial": "mobilenet96-grid2x3-relative-tcn",
    "regularized": "mobilenet96-grid2x3-relative-tcn-regularized",
    "multiview": "mobilenet-two-view-relative-tcn-regularized",
}
LABELS = [
    "No target action",
    "Click one finger",
    "Click two fingers",
    "Throw up",
    "Throw down",
    "Throw left",
    "Throw right",
    "Open twice",
    "Double click one finger",
    "Double click two fingers",
    "Zoom in",
    "Zoom out",
]


def architecture_name(value):
    value = ARCHITECTURES.get(value, value)
    if value not in ARCHITECTURES.values():
        raise ValueError("Unsupported model architecture: " + str(value))
    return value


class Features(nn.Module):
    def __init__(self, pretrained=True, architecture="global"):
        super().__init__()
        self.architecture = architecture_name(architecture)
        self.features = mobilenet_v3_small(
            weights=MobileNet_V3_Small_Weights.IMAGENET1K_V1 if pretrained else None
        ).features
        self.pool = nn.AdaptiveAvgPool2d(1)

    @property
    def prefix_shape(self):
        return (
            (2, 96, 7, 7)
            if self.architecture == ARCHITECTURES["multiview"]
            else (96, 7, 7)
        )

    @staticmethod
    def crop(x):
        return torch.nn.functional.interpolate(
            x[:, :, 72:196, 40:184],
            size=(224, 224),
            mode="bilinear",
            align_corners=False,
        )

    def prefix(self, x):
        if self.architecture == ARCHITECTURES["multiview"]:
            n = x.shape[0]
            y = self.features[:-2](torch.cat([x, self.crop(x)], 0))
            return y.reshape(2, n, 96, 7, 7).transpose(0, 1)
        return self.features[:-2](x)

    @staticmethod
    def grid(x):
        return torch.stack(
            [
                x[:, :, r : r + 4, c : c + 3].mean((2, 3))
                for r in [0, 3]
                for c in [0, 2, 4]
            ],
            2,
        )

    def tail(self, middle):
        if self.architecture == ARCHITECTURES["global"]:
            return self.pool(self.features[-2:](middle)).flatten(1)
        if self.architecture == ARCHITECTURES["multiview"]:
            n = middle.shape[0]
            x = self.features[-2:](middle.flatten(0, 1))
            x = x.reshape(n * 2, 48, 12, 7, 7).mean(2)
            return self.grid(x).reshape(n, 576)
        # Spatial variants retain maps before the final channel expansion.
        return self.grid(self.features[-2](middle)).flatten(1)

    def trainable_tail(self):
        if self.architecture in [
            ARCHITECTURES["spatial"],
            ARCHITECTURES["regularized"],
        ]:
            return self.features[-2:-1]
        return self.features[-2:]

    def forward(self, x):
        return self.tail(self.prefix(x))


class Temporal(nn.Module):
    def __init__(self, window=32, architecture="global"):
        super().__init__()
        self.architecture = architecture_name(architecture)
        if window not in [32, 48]:
            raise ValueError("Unsupported window")
        self.dilations = [1, 2, 4, 8 if window == 32 else 16]
        self.project = nn.Conv2d(576, 128, 1)
        regularized = self.architecture in [
            ARCHITECTURES["regularized"],
            ARCHITECTURES["multiview"],
        ]
        self.dropout = nn.Dropout2d(0.3 if regularized else 0)
        self.layers = nn.ModuleList(
            [nn.Conv2d(128, 128, (1, 3), dilation=(1, d)) for d in self.dilations]
        )
        self.classifier = nn.Conv2d(128, 12, 1)

    def sequence(self, x):
        if self.architecture != ARCHITECTURES["global"]:
            x = x - x[:, :, :, :1]
        x = self.dropout(torch.relu(self.project(x)))
        for layer, dilation in zip(self.layers, self.dilations):
            y = layer(torch.nn.functional.pad(x, (2 * dilation, 0, 0, 0)))
            x = torch.relu(x + self.dropout(y))
        return self.classifier(x)

    def forward(self, x):
        return self.sequence(x)[:, :, 0, -1]
