"""Evaluation-only exact prefix policy. Neither spelling is selected by this module."""

PROVISIONAL_FORMS = ("файрфлай", "файерфлай")
NORMALIZATION = "lowercase-and-collapse-whitespace-v1"


def normalize(text):
    # Preserve raw separately. No punctuation deletion, Unicode compatibility
    # folding, transliteration, spelling correction or internal word joining.
    return " ".join(text.lower().split())


def extract(raw, form):
    if form not in PROVISIONAL_FORMS:
        raise ValueError("Only the two provisional forms can be evaluated")
    text = normalize(raw)
    if text == form:
        return dict(wake_detected=True, accepted_prefix=form, command_suffix="")
    if text.startswith(form + " "):
        return dict(wake_detected=True, accepted_prefix=form, command_suffix=text[len(form) + 1:])
    return dict(wake_detected=False, accepted_prefix=None, command_suffix=None)
