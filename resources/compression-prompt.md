You are an expert technical editor specializing in semantic
compression of technical literature for consumption by large
language models.

TASK

Read the entire attached TXT book and produce a compressed
version optimized for use as reference material in another
LLM's context window.

The objective is maximum reduction in token count while
preserving all substantive technical information.

This is NOT summarization. The compressed document must
retain the technical knowledge of the original book, not
merely its main ideas.

The intended reader is an advanced LLM with extensive
existing knowledge of software engineering, computer
science, and AI. It does not need introductory explanations
of established technical concepts.

COMPRESSION RULES

1. REMOVE HUMAN-ORIENTED PADDING

Eliminate:
- Introductions and motivational explanations.
- Repetition and restatement of previously explained ideas.
- Pedagogical analogies that add no technical information.
- Redundant examples and lengthy illustrations.
- Narrative transitions between topics.
- Chapter previews and repetitive conclusions.
- Explanations of standard technical concepts already
  widely understood by advanced LLMs.
- Historical background unless technically relevant.
- Rhetorical commentary and unnecessary qualifications
  that do not affect technical meaning.

2. PRESERVE TECHNICAL KNOWLEDGE

Retain:
- Definitions and terminology.
- Technical assertions and principles.
- Algorithms, procedures, and execution sequences.
- System architectures and component relationships.
- Implementation details and design decisions.
- Constraints, assumptions, and prerequisites.
- Exceptions, edge cases, and failure conditions.
- Trade-offs and their underlying reasons.
- Causal relationships and dependencies.
- Formulas, constants, parameters, and numerical values.
- API contracts, interfaces, and configuration details.
- Security considerations and operational limitations.
- Examples containing unique technical information.
- Important distinctions between similar concepts.
- Warnings, caveats, and conditional recommendations.

Preserve details even when they appear relatively minor.
Do not assume that a detail is unimportant merely because
it is not central to the chapter's main argument.

3. REWRITE FOR INFORMATION DENSITY

- Express technical statements in concise, precise language.
- Combine related statements when their meaning is preserved.
- Replace lengthy explanations with established technical
  terminology when semantically equivalent.
- Use equations, pseudocode, type signatures, and standard
  technical notation when they improve density.
- Remove redundant words without introducing ambiguity.
- Preserve explicit causal and logical relationships.
- Preserve conditions, exceptions, and scope limitations.
- Avoid repeating definitions already established.
- Do not invent abbreviations or private shorthand.
- Do not sacrifice clarity merely to save tokens.

The result should resemble a highly detailed technical
reference manual, not a conventional textbook.

4. HANDLE CODE CAREFULLY

- Preserve code that communicates essential algorithms,
  implementation details, API usage, or non-obvious behavior.
- Keep retained code semantically identical to the original.
- Remove purely illustrative boilerplate when redundant.
- Preserve comments that contain unique technical information.
- Never replace a concrete algorithm with a vague description.

5. MAINTAIN STRUCTURE AND CONSISTENCY

- Preserve the original chapter structure where practical.
- Organize information under concise technical headings.
- Consolidate repeated concepts across chapters.
- Use consistent terminology throughout the document.
- Preserve important cross-chapter relationships.
- Make technical statements unambiguous without requiring
  access to the original book.
- Keep sufficient structural information to locate the
  corresponding material in the original.

6. STRICT FIDELITY REQUIREMENTS

- Do not introduce information absent from the source.
- Do not add explanations based on external knowledge.
- Do not silently correct the author's technical claims.
- Do not convert recommendations into requirements.
- Do not convert possibilities into certainties.
- Do not remove qualifiers that change meaning.
- Do not generalize specific statements beyond their
  original scope.
- Preserve disagreements, alternative approaches, and
  unresolved questions presented by the author.
- If two sections contradict each other, preserve the
  distinction rather than inventing a reconciliation.

7. COMPRESSION PRIORITY

Optimize in this order:

1. Technical accuracy and factual fidelity.
2. Preservation of independently useful information.
3. Preservation of relationships and dependencies.
4. Clarity for an advanced LLM.
5. Minimum token count.

Compress aggressively where information is redundant.
Compress conservatively where information is unique.

Do not target a fixed compression ratio. The achievable
compression depends on the original text.

FINAL VERIFICATION

Before producing the result, review the original content
against the compressed representation.

Check for:
- Missing technical facts.
- Omitted conditions and exceptions.
- Lost implementation details.
- Altered meanings.
- Missing dependencies or causal relationships.
- Important examples whose unique information was lost.
- Technical distinctions accidentally merged together.

Restore any information lost during compression.

OUTPUT

Produce the complete compressed book as a single TXT document.

Preserve chapter headings and use concise section headings.
Use Markdown-compatible plain-text formatting for readability.
Do not include commentary about the compression process.

If file creation is available, save the result as
book_compressed.txt.

If the complete output exceeds available generation limits,
explicitly report that limitation rather than silently
omitting or truncating sections.

The final document must be suitable for direct insertion
into another LLM's context window as a technical reference.
