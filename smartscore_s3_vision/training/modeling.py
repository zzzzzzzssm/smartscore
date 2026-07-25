from __future__ import annotations

import timm
import torch
from torch import nn

from training.common import CLASS_NAMES


MODEL_NAME = "mobilenetv2_050.lamb_in1k"
TINY_MODEL_NAME = "tiny_point_cnn"
SUPPORTED_MODEL_NAMES = (
    TINY_MODEL_NAME,
    "mobilenetv2_050.lamb_in1k",
    "mobilenetv2_100.ra_in1k",
    "mobilenetv3_small_050.lamb_in1k",
    "mobilenetv3_small_075.lamb_in1k",
    "mobilenetv3_small_100.lamb_in1k",
)


class ConvBnRelu(nn.Sequential):
    def __init__(self, input_channels: int, output_channels: int, stride: int) -> None:
        super().__init__(
            nn.Conv2d(
                input_channels,
                output_channels,
                kernel_size=3,
                stride=stride,
                padding=1,
                bias=False,
            ),
            nn.BatchNorm2d(output_channels),
            nn.ReLU(inplace=True),
        )


class TinyPointNet(nn.Module):
    """ESP-DL-friendly classifier using only regular convolutions and ReLU."""

    def __init__(self) -> None:
        super().__init__()
        self.features = nn.Sequential(
            ConvBnRelu(3, 12, 2),
            ConvBnRelu(12, 20, 2),
            ConvBnRelu(20, 32, 2),
            ConvBnRelu(32, 48, 2),
            ConvBnRelu(48, 64, 2),
        )
        self.pool = nn.AdaptiveAvgPool2d(1)
        self.classifier = nn.Linear(64, len(CLASS_NAMES))

    def forward(self, inputs: torch.Tensor) -> torch.Tensor:
        features = self.features(inputs)
        pooled = self.pool(features).flatten(1)
        return self.classifier(pooled)

    def get_classifier(self) -> nn.Module:
        return self.classifier


def has_pretrained_weights(model_name: str) -> bool:
    return model_name != TINY_MODEL_NAME


def create_model(*, pretrained: bool, model_name: str = MODEL_NAME) -> nn.Module:
    if model_name not in SUPPORTED_MODEL_NAMES:
        raise ValueError(f"unsupported model: {model_name}")
    if model_name == TINY_MODEL_NAME:
        return TinyPointNet()
    return timm.create_model(model_name, pretrained=pretrained, num_classes=len(CLASS_NAMES))


def freeze_backbone(model: nn.Module) -> None:
    for parameter in model.parameters():
        parameter.requires_grad = False
    classifier = model.get_classifier()
    for parameter in classifier.parameters():
        parameter.requires_grad = True


def unfreeze_all(model: nn.Module) -> None:
    for parameter in model.parameters():
        parameter.requires_grad = True


def load_checkpoint(path: str | bytes | "os.PathLike[str]", device: torch.device) -> tuple[nn.Module, dict]:
    checkpoint = torch.load(path, map_location=device, weights_only=True)
    if tuple(checkpoint.get("class_names", ())) != CLASS_NAMES:
        raise ValueError(
            f"checkpoint class order {checkpoint.get('class_names')} does not match required order {CLASS_NAMES}"
        )
    model_name = checkpoint.get("model_name")
    if model_name not in SUPPORTED_MODEL_NAMES:
        raise ValueError(f"unsupported model in checkpoint: {checkpoint.get('model_name')}")
    model = create_model(pretrained=False, model_name=model_name)
    model.load_state_dict(checkpoint["state_dict"])
    model.to(device)
    model.eval()
    return model, checkpoint


def estimate_macs(model: nn.Module, image_size: int) -> int:
    """Estimate Conv2d/Linear multiply-accumulates for candidate ranking."""

    macs = 0
    hooks: list[torch.utils.hooks.RemovableHandle] = []

    def conv_hook(module: nn.Conv2d, _inputs: tuple[torch.Tensor, ...], output: torch.Tensor) -> None:
        nonlocal macs
        output_height, output_width = output.shape[-2:]
        kernel_height, kernel_width = module.kernel_size
        operations_per_output = kernel_height * kernel_width * module.in_channels // module.groups
        macs += int(output_height * output_width * module.out_channels * operations_per_output)

    def linear_hook(module: nn.Linear, _inputs: tuple[torch.Tensor, ...], output: torch.Tensor) -> None:
        nonlocal macs
        batch_outputs = output.numel() // max(output.shape[0], 1)
        macs += int(batch_outputs * module.in_features)

    for layer in model.modules():
        if isinstance(layer, nn.Conv2d):
            hooks.append(layer.register_forward_hook(conv_hook))
        elif isinstance(layer, nn.Linear):
            hooks.append(layer.register_forward_hook(linear_hook))
    training = model.training
    model.eval()
    with torch.inference_mode():
        model(torch.zeros(1, 3, image_size, image_size))
    model.train(training)
    for hook in hooks:
        hook.remove()
    return macs
