# mod_dav_calendar
This module extends a WebDAV server to add support for CalDAV, the calendaring server
protocol defined in RFC4791.

The CalDAV server extends a standard WebDAV server allowing iCal calender files to
be created and searched for by a calendar client.

This module can also combine multiple iCal resources together as created by a calendar
client at a single URL that can be subscribed to by another calendar client.

Requires Apache httpd v2.4.52 or higher.

# download

RPM Packages are available at
[COPR](https://copr.fedorainfracloud.org/coprs/minfrin/mod_dav_calendar/) for Fedora and OpenSUSE.

```
dnf copr enable minfrin/mod_dav_calendar
dnf install mod_dav_calendar
```

If you need to depend on mod_dav_access, enable it as follows:

```
dnf copr enable minfrin/mod_dav_access
dnf install mod_dav_access
```

Ubuntu packages are available through
[PPA](https://launchpad.net/~minfrin/+archive/ubuntu/apache2/).

# quick configuration

    <IfModule !dav_module>
      LoadModule dav_module modules/mod_dav.so
    </IfModule>
    <IfModule !dav_fs_module>
      LoadModule dav_fs_module modules/mod_dav_fs.so
    </IfModule>
    <IfModule !dav_access_module>
      LoadModule dav_access_module modules/mod_dav_access.so
    </IfModule>
    <IfModule !dav_calendar_module>
      LoadModule dav_calendar_module modules/mod_dav_calendar.so
    </IfModule>

    <Location /principal>

      # limit to logged in users
      AuthType basic
      require valid-user

      DavCalendarHome /calendar/personal/%{escape:%{REMOTE_USER}}/
    </Location>

    # Calendar drive
    Alias /calendar /home/calendar
    <Directory /home/calendar>
      Dav on
      Options +Indexes

      # limit to logged in users
      AuthType basic
      require valid-user

      # Allow every resource in the URL space to be able to query who the current principal is
      DavAccessPrincipalUrl /principal/%{escape:%{REMOTE_USER}}

      DavCalendar on
      DavCalendarHome /calendar/personal/%{escape:%{REMOTE_USER}}/
      DavCalendarProvision /calendar/personal/%{escape:%{REMOTE_USER}}/Home/
      DavCalendarTimezone UTC

    </Directory>
    <DirectoryMatch /home/calendar/personal/(?<USER>[^/]+)>
      # limit so that a user can only see their calendar
      AuthMerging And
      require expr "%{escape:%{REMOTE_USER}} == %{env:MATCH_USER}"
    </DirectoryMatch>

# configuration in more detail

## find the principal URL

When a CalDAV client connects to a CalDAV server, a PROPFIND is sent asking for the logged
in user's principal URL. This URL represents the logged in user, and can be queried for
information about that user.

Some WebDAV providers (like Subversion) provide their own principal URL functionality, while
others (like mod_dav_fs) do not. If needed, use mod_dav_access to define the location of
the principal URL.

    Alias /calendar /home/calendar
    <Directory /home/calendar>
      DavAccessPrincipalUrl /principal/%{escape:%{REMOTE_USER}}
    </Directory>

## find the calendar URL

The next step is perform a PROPFIND against the principal URL to ask for the location of the
user's calendars. These calendar URLs might be unique to a specific user, or might be shared
amongst many users.

    <Location /principal>
      DavCalendarHome /calendar/personal/%{escape:%{REMOTE_USER}}/
    </Location>

## access the calendar

Now we know where the calendar is, we can query the calendar. A calendar is a WebDAV directory
containing iCal files that can be searched using PROPFIND. The calendar client's job is to
create and modify the files, the server is a passive participant.

A calendar client might query the calendar URL for the user's calendars instead of the
pricipal URL, so we define the DavCalendarHome a second time.

Calendar clients expect the collections (the directories) belonging to a calendar to already
exist on the server. Use DavCalendarProvision to autocreate the required directories so that
calendar clients work automatically on first use.

    Alias /calendar /home/calendar
    <Directory /home/calendar>
      Dav on
      DavCalendar on
      DavCalendarHome /calendar/personal/%{escape:%{REMOTE_USER}}/
      DavCalendarProvision /calendar/personal/%{escape:%{REMOTE_USER}}/Home/
      DavCalendarTimezone UTC
    </Directory>

# configuration directives

The *DavCalendar* directive enables support for CALDAV compliant PROPFIND requests to a
given URL space. The directive is 'off' or 'on'.

The *DavCalendarTimezone* directive defines the default timezone to be used for
autoprovisioned calendars. Defaults to UTC.

The *DavCalendarMaxResourceSize* directive limits the size of individual iCal files on the
server. A calendar client will automatically split a calendar over multiple small files to
keep sizes within sensible limits. Defaults to 10MB.

The *DavCalendarHome* directive specifies the location of calendars in this URL space. The
parameter is an expression, which could resolve to an URL unique per user, or to a shared
URL common to many users.

The *DavCalendarProvision* directive defines the URL space of a collection that will be
autocreated on first access. The parameter is an expression that could resolve to a URL
unique to a user, or to a common shared URL.

The *DavCalendarAlias* directive makes a collection of iCal resources available combined
together at a single predictable URL. This can be used to allow a calendar to be subscribed
to at a well defined URL outside the calendar web space.

The *DavCalendarAliasMatch* directive makes a collection of iCal resources available
combined together at a predictable URL. In this directive a regular expression can be used
to match the calendar resources, and an expression can be used to define the final URL.

