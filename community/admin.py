from django.contrib import admin
from community.models import UserQuestion, UserAnswer, UserComment, UserRating

admin.site.register(UserQuestion)
admin.site.register(UserAnswer)
admin.site.register(UserComment)
admin.site.register(UserRating)
