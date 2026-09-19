"""
Schema meta-model for C configuration code generation.
Pure Python 3 standard library (no external dependencies).
"""

import sys
sys.dont_write_bytecode = True

from typing import List, Dict, Optional, Any, Union

class FieldType:
    pass

class PrimitiveType(FieldType):
    def __init__(self, c_type: str, json_getter: str, is_numeric: bool = True):
        self.c_type = c_type
        self.json_getter = json_getter
        self.is_numeric = is_numeric

TYPE_BOOL = PrimitiveType("bool", "parse_json_bool", is_numeric=False)
TYPE_INT = PrimitiveType("int", "parse_json_int", is_numeric=True)
TYPE_INT64 = PrimitiveType("int64_t", "parse_json_int64", is_numeric=True)
TYPE_UINT32 = PrimitiveType("uint32_t", "parse_json_uint32", is_numeric=True)
TYPE_SIZE_T = PrimitiveType("size_t", "parse_json_size_t_strict", is_numeric=True)
TYPE_DOUBLE = PrimitiveType("double", "parse_json_double", is_numeric=True)

class StringType(FieldType):
    def __init__(self, max_length: int = 128):
        self.max_length = max_length
        self.c_type = "char"

class EnumType(FieldType):
    def __init__(
        self,
        name: str,
        c_type: str,
        variants: List[tuple],
        default: Optional[str] = None,
        invalid_val: Optional[str] = None,
        is_external: bool = False,
        guard: Optional[str] = None
    ):
        self.name = name
        self.c_type = c_type
        self.variants = variants  # list of (C_VARIANT, "JsonString", [optional aliases], optional guard)
        self.default = default
        self.invalid_val = invalid_val
        self.is_external = is_external
        self.guard = guard

class ArrayType(FieldType):
    def __init__(self, item_type: FieldType):
        self.item_type = item_type

class NamedMapType(FieldType):
    def __init__(
        self,
        name: str,
        item_c_type: str,
        value_type: FieldType,
        value_field: str,
        has_description: bool = False
    ):
        self.name = name
        self.item_c_type = item_c_type
        self.value_type = value_type
        self.value_field = value_field
        self.has_description = has_description

class VariantRule:
    def __init__(
        self,
        tag_value: Union[str, List[str]],
        allowed_keys: Optional[List[str]] = None,
        required_keys: Optional[List[str]] = None,
        fields: Optional[List[str]] = None,
        one_of: Optional[List[List[str]]] = None,
        one_of_sets: Optional[Dict[str, tuple]] = None,
        any_of_optional: Optional[List[tuple]] = None,
        default_assignments: Optional[Dict[str, Any]] = None,
        sets_field: Optional[tuple] = None,
        profile_field: Optional[str] = None,
        profile_enum: Optional[EnumType] = None,
        profile_fallback_required: Optional[List[str]] = None
    ):
        if isinstance(tag_value, str):
            self.tag_values = [tag_value]
        else:
            self.tag_values = list(tag_value)
        self.allowed_keys = allowed_keys
        self.required_keys = required_keys or []
        self.fields = fields or []
        self.one_of = one_of
        self.one_of_sets = one_of_sets or {}
        self.any_of_optional = any_of_optional
        self.default_assignments = default_assignments or {}
        self.sets_field = sets_field
        self.profile_field = profile_field
        self.profile_enum = profile_enum
        self.profile_fallback_required = profile_fallback_required or []

class StructType(FieldType):
    def __init__(
        self,
        name: str,
        c_type: str,
        fields: List['Field'],
        description: str = "",
        is_external: bool = False,
        strict_unknown_fields: bool = False,
        allowed_extra_keys: Optional[List[str]] = None,
        variant_tag_field: Optional[str] = None,
        variant_rules: Optional[List[VariantRule]] = None,
        nested_objects: Optional[Dict[str, List[str]]] = None,
        guard: Optional[str] = None
    ):
        self.name = name
        self.c_type = c_type
        self.fields = fields
        self.description = description
        self.is_external = is_external
        self.strict_unknown_fields = strict_unknown_fields
        self.allowed_extra_keys = allowed_extra_keys or []
        self.variant_tag_field = variant_tag_field
        self.variant_rules = variant_rules or []
        self.nested_objects = nested_objects or {}
        self.guard = guard

class TaggedUnionType(FieldType):
    def __init__(
        self,
        name: str,
        c_type: str,
        tag_enum: EnumType,
        tag_field: str,
        union_field: str,
        variants: Dict[str, Union[StructType, tuple]],
        extra_fields: Optional[List['Field']] = None,
        extra_union_members: Optional[List[tuple]] = None,
        description: str = "",
        is_external: bool = False,
        requires_parameters_object: bool = False,
        container_allowed_keys: Optional[List[str]] = None,
        missing_params_msg: Optional[str] = None,
        is_flattened: bool = False,
        rejected_variants: Optional[List[str]] = None,
        variant_type_aliases: Optional[Dict[str, tuple]] = None,
        variant_type_serializers: Optional[Dict[str, str]] = None,
        guard: Optional[str] = None
    ):
        self.name = name
        self.c_type = c_type
        self.tag_enum = tag_enum
        self.tag_field = tag_field
        self.union_field = union_field
        self.extra_fields = extra_fields or []
        self.extra_union_members = extra_union_members or []
        self.is_external = is_external
        self.requires_parameters_object = requires_parameters_object
        self.container_allowed_keys = container_allowed_keys
        self.missing_params_msg = missing_params_msg
        self.is_flattened = is_flattened
        self.rejected_variants = rejected_variants or []
        self.variant_type_aliases = variant_type_aliases or {}
        self.variant_type_serializers = variant_type_serializers or {}
        self.guard = guard
        # Normalized variants dict mapping tag -> (field_name, StructType)
        norm_variants = {}
        for k, v in variants.items():
            if isinstance(v, tuple):
                norm_variants[k] = (v[0], v[1])
            else:
                field_name = v.name.replace("_processor_config", "").replace("_config", "")
                norm_variants[k] = (field_name, v)
        self.variants = norm_variants
        self.description = description

class Field:
    def __init__(
        self,
        name: str,
        field_type: FieldType,
        required: bool = False,
        default: Any = None,
        has_flag: bool = False,
        json_key: Optional[str] = None,
        json_object: Optional[str] = None,
        aliases: Optional[List[str]] = None,
        description: str = "",
        getter_default: Any = None,
        allow_null_items: bool = False,
        guard: Optional[str] = None
    ):
        self.name = name
        self.type = field_type
        self.required = required
        self.default = default
        self.has_flag = has_flag
        self.json_key = json_key or name
        self.json_object = json_object
        self.aliases = aliases or []
        self.description = description
        self.getter_default = getter_default
        self.allow_null_items = allow_null_items
        self.guard = guard
