# CHA web application: the route to a working product

A short, non-technical companion to [webapp.md](webapp.md), which argues the
decisions, and [webapp-plan.md](webapp-plan.md), which specifies the work. This
document answers one question: what happens between now and a version of CHA
that can be handed to someone on a laptop?

## Where we are

The engine is finished. `chaweb` already runs conversations, stores them, talks
to the model provider, and answers a complete set of requests over the network —
that part is built and tested. What it hands to a browser today is a single line
of placeholder text.

The design is finished too. [web-ui/](web-ui/README.md) settles what every
screen looks like and how it behaves, and there is a working visual mockup.

So the missing piece is not the product's thinking or its appearance. It is the
page itself: the thing that turns the finished engine and the finished design
into something a person can click. Plus the packaging that lets it be copied to
a laptop and started.

## What gets built

Two parts that already exist as ideas:

**The program.** `chaweb`, the existing native application. It keeps the
conversations, contacts the model, and now also hands out the browser files.

**The page.** A set of files — the layout, the styling, the behavior — that the
browser downloads from the program and runs. It is not a second program and it
does not need to be installed. It is the designed interface, made real.

They talk to each other over the machine's own network connection. The customer
sees one application.

And two folders, kept firmly apart:

**The application folder** holds the program, the page files, and the launcher.
It is disposable. A new version of CHA replaces it completely.

**The workspace folder** holds everything belonging to the person using it:
their provider key, their characters and personas, their forums, every stored
conversation, and the log. It is never touched by an upgrade.

The separation is the whole reason upgrading is safe. If the conversations sat
inside the application folder, shipping a new version would delete them. The
customer chooses where the workspace lives when they first set CHA up, so it
can go anywhere, including a shared drive.

Starting CHA is a small script that sits in the application folder next to the
program. Its first three lines are the address to listen on, the port, and
where the workspace is; editing those three lines is what setting CHA up
means. The workspace has its own settings file saying which model to talk to
and how to log, and the provider key goes in a plain text file beside it.

That is the whole of configuration. There is no settings screen, nothing asks
for a password, and an application settings file exists for anyone who would
rather not edit a script.

## The route, in seven steps

The steps are ordered so that each one finishes with everything building and
every test passing. Work can stop between any two of them without leaving a mess
behind. The first two are work on the existing program; the middle four build
the page; the last one makes it a product.

**1. Tell the program where things are.** Today `chaweb` looks for its
settings, its logs, and its conversations in whichever folder it was started
from. That is fine when a developer starts it by hand from the right place, and
it breaks the moment someone launches it from a shortcut or a different drive.
It learns two folders instead — the program's own folder and the workspace,
described below — and stops caring where it was started from. The same step
makes it reachable from other machines on the network, which needs a small
change to how it checks incoming requests.
This step also makes the build assemble a `bin/` folder in the project laid out
exactly like a real installation, with the same starting script in it, so every
later step can be tried the way a customer would run it rather than the way a
developer usually does. That script already exists; what it needs is a program
that accepts the settings it passes.
*Finished when:* it starts correctly from anywhere with the two folders in
unrelated places, another computer on the network can reach it, and running the
script in `bin/` starts CHA.

**2. Let the program hand out the page.** The placeholder response is replaced
by proper file serving: only files that genuinely belong to the application,
each sent with the right description so the browser knows what it received,
cached sensibly, and locked down so the page cannot pull in anything from the
Internet.
*Finished when:* a hand-written test page loads in a real browser, both at the
main address and at a conversation link.

**3. Build the shell of the real page.** The frontend project is created and the
mockup becomes real components — sidebar, main area, chat composer — with its
placeholder text stripped out and its externally-loaded pieces replaced by local
ones. This step also sets up the day-to-day editing loop, where a styling change
appears in the browser immediately, and the automated browser testing that every
later step leans on.
*Finished when:* the designed interface appears at both laptop and phone widths
and the sidebar opens and closes correctly. It has no real content yet.

**4. Fill it with real content.** The page starts asking the program for things:
personas, characters, a character's full description, forums. Everything here is
read-only, so it is the safest place to prove that the two halves talk to each
other properly.
*Finished when:* navigating the interface shows the actual contents of a real
workspace.

**5. Sessions and addresses.** Browsing a forum's stored conversations, naming
and creating a new one, opening it, and finding it again in the Recent list.
Each open conversation gets its own address, so it can be reloaded, bookmarked,
or sent to someone.
*Finished when:* a conversation can be created, opened, left, returned to, and
survives a page reload.

**6. The live conversation.** The part that makes it CHA: typing a message,
watching the reply arrive as it is written, stopping it part-way, and choosing
which character answers. This step also carries the unglamorous work that
decides whether the application feels solid — recovering by itself when the
connection drops because the laptop slept or the network changed, and saying
something clear when a conversation is already open in another window.
*Finished when:* a full conversation works end to end, including after the
machine has been asleep long enough for the program to have put it away.

**7. Make it something you can hand over.** Every "what if" is given a
sensible answer: nothing loaded yet, no conversations in this forum, request
failed, connection dropped, too many conversations open at once. Then one
command builds the application folder, the starting script grows up into a
proper launcher that also opens the browser, and the result is tested on a
clean machine that has none of the development tools on it — including a test
that installing a newer version over an existing workspace leaves the
conversations, characters, and key untouched.
*Finished when:* the folder can be copied to a laptop, pointed at a workspace,
started, used, and shut down.

## What has been decided

**It is reachable over the network.** Not just on the machine running it, so a
second computer can open it. There is no password and no encryption: anyone who
can reach it can read and continue the conversations. That is an accepted
trade for a trusted home or office network, and it means the application must
not be exposed to the open Internet.

**One window per conversation.** Two people, or two browser tabs, can use CHA at
the same time as long as they are in different conversations. Opening the *same*
conversation twice tells the second window that it is already open elsewhere,
rather than quietly showing a stale page. Letting several windows watch one
conversation live is a change inside the program and is not part of this
release.

**Linux comes first.** The first launcher and the final clean-machine test are
Linux; macOS and Windows follow, in that order. The program is written from the
start so that all three work.

**There is no Settings screen.** Not a placeholder, not an informational page —
it is gone, along with the gear button in the corner of the sidebar. Nothing in
it could have done anything yet, and a control that looks usable but is not is
worse than no control.

**Nothing is fixed to one address or port.** Both are settings, so CHA can move
out of the way of anything else already running on the machine. If the port it
is told to use is taken, it says so and stops instead of guessing.

**Preparing the workspace is somebody else's problem.** These steps assume a
workspace already exists, with its characters and personas in it. Building or
generating one is not part of this work.

## What is deliberately left out

File attachments, because the program has no support for them. Any way to
create or edit personas, characters, or forums from the browser, or to prepare
a workspace. Installers, automatic updates, and background services. Anything
that would make network access genuinely safe — passwords, encryption, separate
accounts — which is a project of its own.

Every one of these is left out for the same reason: the goal is a working
application soon, and each of them is a feature in its own right rather than a
finishing touch on this one.
